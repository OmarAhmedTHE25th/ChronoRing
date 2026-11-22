use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int};
use std::ptr;

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct EventFrame {
    pub timestamp_ns: u64,
    pub sequence_id: u64,
    pub detector_id: u32,
    pub channel_id: u32,
    pub energy_mev: f32,
    pub timing_ps: f32,
    pub pos_x_mm: f32,
    pub pos_y_mm: f32,
    pub pos_z_mm: f32,
    pub pileup_score: f32,
    pub quality_flags: u32,
    pub multiplicity: u16,
    pub trigger_mask: u16,
    pub payload_crc: u32,
    pub reserved: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct TriggerConfig {
    pub min_energy_mev: f32,
    pub max_energy_mev: f32,
    pub max_abs_timing_ps: f32,
    pub max_pileup_score: f32,
    pub required_flags: u32,
    pub forbidden_flags: u32,
    pub min_multiplicity: u16,
    pub required_trigger_mask: u16,
}

#[repr(C)]
pub struct ChronoringSpsc {
    _private: [u8; 0],
}

#[repr(C)]
pub struct ChronoringSink {
    _private: [u8; 0],
}

#[link(name = "chronoring_core")]
extern "C" {
    fn chronoring_version() -> *const c_char;
    fn chronoring_default_trigger_config() -> TriggerConfig;
    fn chronoring_crc32c(data: *const u8, len: usize, initial: u32) -> u32;
    fn chronoring_spsc_create() -> *mut ChronoringSpsc;
    fn chronoring_spsc_destroy(ring: *mut ChronoringSpsc);
    fn chronoring_spsc_try_push(ring: *mut ChronoringSpsc, frame: *const EventFrame) -> c_int;
    fn chronoring_spsc_try_pop(ring: *mut ChronoringSpsc, out_frame: *mut EventFrame) -> c_int;
    fn chronoring_sink_create() -> *mut ChronoringSink;
    fn chronoring_sink_open(
        sink: *mut ChronoringSink,
        path: *const c_char,
        cap: usize,
        mask: u16,
        compress: c_int,
    ) -> c_int;
    fn chronoring_sink_write_batch(
        sink: *mut ChronoringSink,
        frames: *const EventFrame,
        count: usize,
    ) -> usize;
    fn chronoring_sink_close(sink: *mut ChronoringSink);
    fn chronoring_sink_destroy(sink: *mut ChronoringSink);
}

pub fn version() -> &'static str {
    unsafe { CStr::from_ptr(chronoring_version()) }.to_str().unwrap_or("unknown")
}

pub fn default_trigger_config() -> TriggerConfig {
    unsafe { chronoring_default_trigger_config() }
}

pub fn crc32c(bytes: &[u8]) -> u32 {
    unsafe { chronoring_crc32c(bytes.as_ptr(), bytes.len(), 0) }
}

pub struct SpscRing {
    ptr: *mut ChronoringSpsc,
}

impl SpscRing {
    pub fn new() -> Option<Self> {
        let p = unsafe { chronoring_spsc_create() };
        if p.is_null() { None } else { Some(Self { ptr: p }) }
    }

    pub fn try_push(&mut self, frame: &EventFrame) -> bool {
        unsafe { chronoring_spsc_try_push(self.ptr, frame as *const EventFrame) != 0 }
    }

    pub fn try_pop(&mut self) -> Option<EventFrame> {
        let mut out = EventFrame::default();
        if unsafe { chronoring_spsc_try_pop(self.ptr, &mut out as *mut EventFrame) != 0 } {
            Some(out)
        } else {
            None
        }
    }
}

impl Drop for SpscRing {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            unsafe { chronoring_spsc_destroy(self.ptr) };
            self.ptr = ptr::null_mut();
        }
    }
}

pub struct ColumnarSink {
    ptr: *mut ChronoringSink,
}

impl ColumnarSink {
    pub fn open(path: &str, compress: bool) -> Result<Self, &'static str> {
        let cpath = CString::new(path).map_err(|_| "Invalid path")?;
        let ptr = unsafe { chronoring_sink_create() };
        if ptr.is_null() {
            return Err("Allocation failed");
        }
        let ok = unsafe { chronoring_sink_open(ptr, cpath.as_ptr(), 64, 0x1F, if compress { 1 } else { 0 }) };
        if ok == 0 {
            unsafe { chronoring_sink_destroy(ptr) };
            return Err("Failed to open sink");
        }
        Ok(Self { ptr })
    }

    pub fn write_batch(&mut self, frames: &[EventFrame]) -> usize {
        unsafe { chronoring_sink_write_batch(self.ptr, frames.as_ptr(), frames.len()) }
    }
}

impl Drop for ColumnarSink {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            unsafe {
                chronoring_sink_close(self.ptr);
                chronoring_sink_destroy(self.ptr);
            }
            self.ptr = ptr::null_mut();
        }
    }
}

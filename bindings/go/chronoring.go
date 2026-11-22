package chronoring

/*
#cgo CFLAGS: -I../../include
#cgo LDFLAGS: -L../../build -lchronoring_core
#include "chronoring/c_api.h"
#include <stdlib.h>
*/
import "C"
import "unsafe"

type EventFrame = C.chronoring_event_frame_t
type TriggerConfig = C.chronoring_trigger_config_t

func Version() string {
	return C.GoString(C.chronoring_version())
}

func DefaultTriggerConfig() TriggerConfig {
	return C.chronoring_default_trigger_config()
}

func CRC32C(data []byte) uint32 {
	if len(data) == 0 {
		return 0
	}
	return uint32(C.chronoring_crc32c(unsafe.Pointer(&data[0]), C.size_t(len(data)), 0))
}

type SpscRing struct {
	ptr *C.chronoring_spsc_t
}

func NewSpscRing() *SpscRing {
	return &SpscRing{ptr: C.chronoring_spsc_create()}
}

func (r *SpscRing) Close() {
	if r.ptr != nil {
		C.chronoring_spsc_destroy(r.ptr)
		r.ptr = nil
	}
}

func (r *SpscRing) TryPush(ev *EventFrame) bool {
	return C.chronoring_spsc_try_push(r.ptr, ev) != 0
}

func (r *SpscRing) TryPop(out *EventFrame) bool {
	return C.chronoring_spsc_try_pop(r.ptr, out) != 0
}

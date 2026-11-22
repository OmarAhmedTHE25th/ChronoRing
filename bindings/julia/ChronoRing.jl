module ChronoRing

export EventFrame, version, crc32c, SpscRing, try_push!, try_pop!, close!

const LIBCHRONORING = get(ENV, "CHRONORING_LIB", "libchronoring_core")

struct EventFrame
    timestamp_ns::UInt64
    sequence_id::UInt64
    detector_id::UInt32
    channel_id::UInt32
    energy_mev::Float32
    timing_ps::Float32
    pos_x_mm::Float32
    pos_y_mm::Float32
    pos_z_mm::Float32
    pileup_score::Float32
    quality_flags::UInt32
    multiplicity::UInt16
    trigger_mask::UInt16
    payload_crc::UInt32
    reserved::UInt32
end

version() = unsafe_string(ccall((:chronoring_version, LIBCHRONORING), Cstring, ()))

function crc32c(data::AbstractVector{UInt8}, initial::UInt32 = UInt32(0))::UInt32
    ccall((:chronoring_crc32c, LIBCHRONORING), UInt32,
          (Ptr{UInt8}, Csize_t, UInt32), data, length(data), initial)
end

mutable struct SpscRing
    handle::Ptr{Cvoid}
    function SpscRing()
        ptr = ccall((:chronoring_spsc_create, LIBCHRONORING), Ptr{Cvoid}, ())
        ring = new(ptr)
        finalizer(close!, ring)
        return ring
    end
end

function close!(ring::SpscRing)
    if ring.handle != C_NULL
        ccall((:chronoring_spsc_destroy, LIBCHRONORING), Cvoid, (Ptr{Cvoid},), ring.handle)
        ring.handle = C_NULL
    end
end

function try_push!(ring::SpscRing, ev::EventFrame)::Bool
    ref = Ref(ev)
    return ccall((:chronoring_spsc_try_push, LIBCHRONORING), Cint,
                 (Ptr{Cvoid}, Ref{EventFrame}), ring.handle, ref) != 0
end

function try_pop!(ring::SpscRing)::Union{EventFrame, Nothing}
    out = Ref{EventFrame}()
    ok = ccall((:chronoring_spsc_try_pop, LIBCHRONORING), Cint,
               (Ptr{Cvoid}, Ref{EventFrame}), ring.handle, out)
    return ok != 0 ? out[] : nothing
end

end # module ChronoRing

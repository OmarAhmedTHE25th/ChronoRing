package io.chronoring;

import java.lang.foreign.*;
import java.lang.invoke.MethodHandle;

/** Java 22+ Foreign Function & Memory (FFM) bindings atop libchronoring_core C ABI. */
public final class ChronoRing implements AutoCloseable {
    private static final Linker LINKER = Linker.nativeLinker();
    private static final SymbolLookup LOOKUP = SymbolLookup.libraryLookup(
        System.mapLibraryName("chronoring_core"), Arena.global());

    private static final MethodHandle VERSION_FN = LINKER.downcallHandle(
        LOOKUP.find("chronoring_version").orElseThrow(),
        FunctionDescriptor.of(ValueLayout.ADDRESS));

    private static final MethodHandle SPSC_CREATE = LINKER.downcallHandle(
        LOOKUP.find("chronoring_spsc_create").orElseThrow(),
        FunctionDescriptor.of(ValueLayout.ADDRESS));

    private static final MethodHandle SPSC_DESTROY = LINKER.downcallHandle(
        LOOKUP.find("chronoring_spsc_destroy").orElseThrow(),
        FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));

    private MemorySegment handle;

    public ChronoRing() throws Throwable {
        this.handle = (MemorySegment) SPSC_CREATE.invokeExact();
    }

    public static String version() throws Throwable {
        MemorySegment ptr = (MemorySegment) VERSION_FN.invokeExact();
        return ptr.reinterpret(64).getString(0);
    }

    @Override
    public void close() throws Exception {
        if (handle != null && !handle.equals(MemorySegment.NULL)) {
            try {
                SPSC_DESTROY.invokeExact(handle);
            } catch (Throwable t) {
                throw new RuntimeException(t);
            }
            handle = MemorySegment.NULL;
        }
    }
}

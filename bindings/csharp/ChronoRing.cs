using System;
using System.Runtime.InteropServices;

namespace ChronoRing {
    [StructLayout(LayoutKind.Sequential)]
    public struct EventFrame {
        public ulong TimestampNs;
        public ulong SequenceId;
        public uint DetectorId;
        public uint ChannelId;
        public float EnergyMev;
        public float TimingPs;
        public float PosXMm;
        public float PosYMm;
        public float PosZMm;
        public float PileupScore;
        public uint QualityFlags;
        public ushort Multiplicity;
        public ushort TriggerMask;
        public uint PayloadCrc;
        public uint Reserved;
    }

    public static unsafe class Native {
        private const string LibName = "chronoring_core";

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr chronoring_version();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint chronoring_crc32c(byte[] data, UIntPtr len, uint initial);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr chronoring_spsc_create();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void chronoring_spsc_destroy(IntPtr ring);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int chronoring_spsc_try_push(IntPtr ring, ref EventFrame frame);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int chronoring_spsc_try_pop(IntPtr ring, out EventFrame frame);

        public static string Version() => Marshal.PtrToStringAnsi(chronoring_version()) ?? "unknown";
    }
}

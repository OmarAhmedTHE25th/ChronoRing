classdef ChronoRing < handle
    % MATLAB FFI wrapper around libchronoring_core C ABI via loadlibrary/calllib.
    properties (Access = private)
        LibName = 'chronoring_core'
        SinkPtr = []
    end

    methods
        function obj = ChronoRing(libPath, headerPath)
            if ~libisloaded(obj.LibName)
                loadlibrary(libPath, headerPath);
            end
        end

        function v = version(obj)
            v = calllib(obj.LibName, 'chronoring_version');
        end

        function crc = crc32c(obj, bytes)
            u8 = uint8(bytes);
            crc = calllib(obj.LibName, 'chronoring_crc32c', u8, numel(u8), uint32(0));
        end

        function openSink(obj, outPath, enableLz4)
            obj.SinkPtr = calllib(obj.LibName, 'chronoring_sink_create');
            calllib(obj.LibName, 'chronoring_sink_open', obj.SinkPtr, outPath, 64, uint16(31), int32(enableLz4));
        end

        function delete(obj)
            if ~isempty(obj.SinkPtr)
                calllib(obj.LibName, 'chronoring_sink_close', obj.SinkPtr);
                calllib(obj.LibName, 'chronoring_sink_destroy', obj.SinkPtr);
                obj.SinkPtr = [];
            end
        end
    end
end

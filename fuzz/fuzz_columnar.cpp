#include "chronoring/columnar_sink.hpp"
#include "chronoring/event_frame.hpp"
#include "chronoring/simd_trigger.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size >= sizeof(chronoring::ColumnarPage)) {
        chronoring::ColumnarPage page{};
        std::memcpy(static_cast<void*>(&page), data, sizeof(chronoring::ColumnarPage));
        (void)chronoring::verify_page_crc(page);
    }

    const std::string path =
        (std::filesystem::temp_directory_path() /
         ("chronoring_fuzz_" + std::to_string(static_cast<unsigned long>(::getpid())) + ".crng"))
            .string();

    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (ofs.is_open() && size > 0) {
            ofs.write(reinterpret_cast<const char*>(data),
                      static_cast<std::streamsize>(size));
        }
    }

    chronoring::ColumnarReader reader(path);
    if (reader.is_open()) {
        (void)reader.verify_all_pages();
        (void)reader.page_index();
        (void)reader.replay_filtered(chronoring::TriggerConfig{});
    }
    reader.close();

    std::error_code ec;
    std::filesystem::remove(path, ec);
    return 0;
}

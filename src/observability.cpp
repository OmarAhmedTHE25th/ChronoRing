#include "chronoring/observability.hpp"

#include <iomanip>
#include <sstream>

namespace chronoring {

namespace {

std::string hex_u64(std::uint64_t val, int width) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(width) << val;
    return oss.str();
}

} // namespace

std::string export_otlp_metrics_json(const PipelineStats& stats,
                                     std::string_view service_name) {
    std::ostringstream oss;
    oss << "{\n"
        << "  \"resourceMetrics\": [{\n"
        << "    \"resource\": {\"attributes\": [{\"key\": \"service.name\", \"value\": {\"stringValue\": \""
        << service_name << "\"}}]},\n"
        << "    \"scopeMetrics\": [{\n"
        << "      \"scope\": {\"name\": \"chronoring.telemetry\", \"version\": \"" << version() << "\"},\n"
        << "      \"metrics\": [\n"
        << "        {\"name\": \"chronoring.events.submitted\", \"sum\": {\"isMonotonic\": true, \"aggregationTemporality\": 2, \"dataPoints\": [{\"asInt\": \"" << stats.submitted_events << "\"}]}},\n"
        << "        {\"name\": \"chronoring.events.passed\", \"sum\": {\"isMonotonic\": true, \"aggregationTemporality\": 2, \"dataPoints\": [{\"asInt\": \"" << stats.passed_events << "\"}]}},\n"
        << "        {\"name\": \"chronoring.events.rejected\", \"sum\": {\"isMonotonic\": true, \"aggregationTemporality\": 2, \"dataPoints\": [{\"asInt\": \"" << stats.rejected_events << "\"}]}},\n"
        << "        {\"name\": \"chronoring.events.dropped\", \"sum\": {\"isMonotonic\": true, \"aggregationTemporality\": 2, \"dataPoints\": [{\"asInt\": \"" << stats.dropped_events << "\"}]}},\n"
        << "        {\"name\": \"chronoring.latency.p99_ns\", \"gauge\": {\"dataPoints\": [{\"asInt\": \"" << stats.p99_ns << "\"}]}},\n"
        << "        {\"name\": \"chronoring.ring.fill_ratio\", \"gauge\": {\"dataPoints\": [{\"asDouble\": " << std::fixed << std::setprecision(4) << stats.ring_fill_ratio << "}]}},\n"
        << "        {\"name\": \"chronoring.throughput.meps\", \"gauge\": {\"dataPoints\": [{\"asDouble\": " << std::fixed << std::setprecision(3) << stats.throughput_meps << "}]}}\n"
        << "      ]\n"
        << "    }]\n"
        << "  }]\n"
        << "}\n";
    return oss.str();
}

std::string export_otlp_traces_json(std::span<const TraceSpanEvent> spans,
                                    std::string_view service_name) {
    std::ostringstream oss;
    oss << "{\n"
        << "  \"resourceSpans\": [{\n"
        << "    \"resource\": {\"attributes\": [{\"key\": \"service.name\", \"value\": {\"stringValue\": \""
        << service_name << "\"}}]},\n"
        << "    \"scopeSpans\": [{\n"
        << "      \"scope\": {\"name\": \"chronoring.line_rate_tracer\", \"version\": \"" << version() << "\"},\n"
        << "      \"spans\": [\n";

    for (std::size_t i = 0; i < spans.size(); ++i) {
        const TraceSpanEvent& sp = spans[i];
        const std::uint64_t end_ns = sp.start_ns + sp.duration_ns;
        const std::string trace_hex =
            "0000000000000000" + hex_u64(static_cast<std::uint64_t>(sp.trace_id), 16);
        const std::string span_hex =
            hex_u64((static_cast<std::uint64_t>(sp.span_id) << 32U) | static_cast<std::uint64_t>(i + 1U), 16);

        oss << "        {\n"
            << "          \"traceId\": \"" << trace_hex << "\",\n"
            << "          \"spanId\": \"" << span_hex << "\",\n"
            << "          \"name\": \"" << trace_stage_name(static_cast<TraceStage>(sp.stage)) << "\",\n"
            << "          \"kind\": 1,\n"
            << "          \"startTimeUnixNano\": \"" << sp.start_ns << "\",\n"
            << "          \"endTimeUnixNano\": \"" << end_ns << "\",\n"
            << "          \"attributes\": [\n"
            << "            {\"key\": \"chronoring.batch_size\", \"value\": {\"intValue\": \"" << sp.batch_size << "\"}},\n"
            << "            {\"key\": \"chronoring.flags\", \"value\": {\"intValue\": \"" << sp.flags << "\"}}\n"
            << "          ]\n"
            << "        }" << ((i + 1 < spans.size()) ? "," : "") << "\n";
    }

    oss << "      ]\n"
        << "    }]\n"
        << "  }]\n"
        << "}\n";
    return oss.str();
}

std::string export_prometheus_observability(const PipelineStats& stats,
                                            std::uint64_t anomalies_detected,
                                            std::size_t tuned_batch_size,
                                            std::uint64_t config_generation) {
    std::ostringstream oss;
    oss << to_prometheus_text(stats)
        << "# HELP chronoring_anomalies_detected_total Streaming anomalies detected by control plane\n"
        << "# TYPE chronoring_anomalies_detected_total counter\n"
        << "chronoring_anomalies_detected_total " << anomalies_detected << "\n"
        << "# HELP chronoring_autotuner_batch_size Current auto-tuned processing batch size\n"
        << "# TYPE chronoring_autotuner_batch_size gauge\n"
        << "chronoring_autotuner_batch_size " << tuned_batch_size << "\n"
        << "# HELP chronoring_config_generation Active zero-drop live configuration generation\n"
        << "# TYPE chronoring_config_generation counter\n"
        << "chronoring_config_generation " << config_generation << "\n";
    return oss.str();
}

} // namespace chronoring

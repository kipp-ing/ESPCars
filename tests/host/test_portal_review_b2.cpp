#include "harness.h"
#include "portal_core.h"

#include <cmath>
#include <cstdlib>
#include <limits>

using namespace esphome::portal::core;

namespace {
bool collect(void *ctx, const char *data, size_t size) {
  static_cast<std::string *>(ctx)->append(data, size);
  return true;
}
}  // namespace

TEST(portal_b2_large_finite_numbers_keep_magnitude) {
  for (float value : {1e14f, 1e20f, -1e30f, std::numeric_limits<float>::max()}) {
    for (int8_t decimals : {0, 6}) {
      char buf[64];
      std::string output;
      ChunkWriter writer;
      writer.init(buf, sizeof(buf), collect, &output);
      CHECK(writer.number_float(value, decimals));
      CHECK(writer.finish());
      char *end = nullptr;
      double parsed = std::strtod(output.c_str(), &end);
      CHECK(end != output.c_str() && *end == '\0');
      CHECK_EQ(static_cast<float>(parsed), value);
    }
  }
}

TEST(portal_b2_nonfinite_series_are_sna_without_float_to_int_cast) {
  for (float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity()}) {
    SeriesDef series{};
    series.key = "samples";
    series.label = "Samples";
    series.count = 1;
    series.ctx = &value;
    series.element = +[](void *ctx, uint16_t, SeriesElement *out) {
      out->cls = 'r';
      out->has_value = true;
      out->value = *static_cast<float *>(ctx);
      return true;
    };
    char buf[512];
    std::string output;
    ChunkWriter writer;
    writer.init(buf, sizeof(buf), collect, &output);
    CHECK(detail::write_series(writer, series));
    CHECK(writer.finish());
    CHECK(output.find("\"values\":[null],\"class\":\"s\"") != std::string::npos);
  }
}

TEST(portal_b2_json_nul_cannot_alias_key_or_select_option) {
  const char *options[] = {"run"};
  ActionDef def{};
  def.control = Control::SELECT;
  def.options = options;
  def.option_count = 1;
  ActionSample sample;
  sample.enabled = true;
  for (const char *body : {R"({"value\u0000suffix":"run"})", R"({"value":"run\u0000suffix"})",
                           R"({"ignored":{"x":"\u0000"},"value":"run"})"}) {
    const auto result = evaluate_action_json(def, sample, false, body, std::strlen(body));
    CHECK_EQ(result.verdict, Verdict::REFUSE);
    CHECK_EQ(result.code, ActionCode::INVALID_BODY);
  }
  const char *body = R"({"value":"run"})";
  CHECK_EQ(evaluate_action_json(def, sample, false, body, std::strlen(body)).verdict, Verdict::ACCEPT);
}

TEST(portal_b2_oversized_utf8_request_does_not_echo_a_partial_character) {
  ActionDef def{};
  def.control = Control::SELECT;
  ActionSample sample;
  sample.enabled = true;
  std::string text(62, 'x');
  text += "é";
  std::string body = "{\"value\":\"" + text + "\"}";
  for (auto result : {evaluate_action_json(def, sample, false, body.c_str(), body.size()),
                      evaluate_action_text(def, sample, false, true, text.c_str(), text.size())}) {
    CHECK_EQ(result.verdict, Verdict::REFUSE);
    CHECK_EQ(result.value.kind, ValueKind::NONE);
  }
}

TEST(portal_b2_form_decoder_rejects_nul_and_overflow_without_a_prefix) {
  char out[ACTION_TEXT_MAX]{};
  for (const std::string &body : {std::string("value=run%00suffix"), std::string("value=run\0suffix", 16),
                                  "value=" + std::string(62, 'x') + "%C3%A9"}) {
    std::strcpy(out, "previous");
    CHECK(!form_value(body.c_str(), body.size(), out, sizeof(out)));
    CHECK_EQ(out[0], '\0');
  }
}

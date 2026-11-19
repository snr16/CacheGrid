#include "cachegrid/Resp.h"

#include <gtest/gtest.h>

using namespace cachegrid::resp;
using Args = std::vector<std::string>;

namespace {

// Test fixture limits (independent of shipped config).
const Limits kLimits{1024, 16};

ParseResult parse(std::string_view s) { return parse_command(s, kLimits); }

}  // namespace

// ---- Array (multibulk) commands -----------------------------------------------

TEST(RespParse, CompleteArrayCommand) {
  const std::string req = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nhello\r\n";
  const auto r = parse(req);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  EXPECT_EQ(r.args, (Args{"SET", "k", "hello"}));
  EXPECT_EQ(r.consumed, req.size());
}

TEST(RespParse, EveryPrefixIsIncomplete) {
  // Network data arrives in arbitrary pieces: every cut point must be safe.
  const std::string req = "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n";
  for (std::size_t n = 0; n < req.size(); ++n) {
    EXPECT_EQ(parse(req.substr(0, n)).status, ParseStatus::Incomplete) << "prefix length " << n;
  }
  EXPECT_EQ(parse(req).status, ParseStatus::Ok);
}

TEST(RespParse, BinarySafeValues) {
  const std::string value("a\r\nb\0c", 6);
  const std::string req = "*2\r\n$4\r\nECHO\r\n$6\r\n" + value + "\r\n";
  const auto r = parse(req);
  ASSERT_EQ(r.status, ParseStatus::Ok);
  EXPECT_EQ(r.args[1], value);
}

TEST(RespParse, PipelinedCommandsParsedOneAtATime) {
  const std::string req = "*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n";
  const auto r1 = parse(req);
  ASSERT_EQ(r1.status, ParseStatus::Ok);
  const auto r2 = parse(std::string_view(req).substr(r1.consumed));
  ASSERT_EQ(r2.status, ParseStatus::Ok);
  EXPECT_EQ(r1.consumed + r2.consumed, req.size());
}

TEST(RespParse, EmptyArrayIsIgnorable) {
  const auto r = parse("*0\r\n");
  EXPECT_EQ(r.status, ParseStatus::Ok);
  EXPECT_TRUE(r.args.empty());
  EXPECT_EQ(r.consumed, 4u);
}

TEST(RespParse, EmptyBulkString) {
  const auto r = parse("*2\r\n$3\r\nGET\r\n$0\r\n\r\n");
  ASSERT_EQ(r.status, ParseStatus::Ok);
  EXPECT_EQ(r.args, (Args{"GET", ""}));
}

// ---- Inline commands -------------------------------------------------------------

TEST(RespParse, InlineCommand) {
  const auto r = parse("SET  key\tvalue\r\n");
  ASSERT_EQ(r.status, ParseStatus::Ok);
  EXPECT_EQ(r.args, (Args{"SET", "key", "value"}));
}

TEST(RespParse, InlineWithoutCarriageReturn) {
  const auto r = parse("PING\n");
  ASSERT_EQ(r.status, ParseStatus::Ok);
  EXPECT_EQ(r.args, (Args{"PING"}));
}

TEST(RespParse, InlineIncompleteWithoutNewline) {
  EXPECT_EQ(parse("PIN").status, ParseStatus::Incomplete);
}

TEST(RespParse, BlankLineIsIgnorable) {
  const auto r = parse("\r\n");
  EXPECT_EQ(r.status, ParseStatus::Ok);
  EXPECT_TRUE(r.args.empty());
}

// ---- Malformed input ---------------------------------------------------------------

TEST(RespParse, NonNumericArrayLength) {
  EXPECT_EQ(parse("*x\r\n").status, ParseStatus::Error);
}

TEST(RespParse, WrongTypeInsideArray) {
  const auto r = parse("*1\r\n+PING\r\n");
  EXPECT_EQ(r.status, ParseStatus::Error);
  EXPECT_NE(r.error.find("expected '$'"), std::string::npos);
}

TEST(RespParse, NegativeBulkLength) {
  EXPECT_EQ(parse("*1\r\n$-5\r\n").status, ParseStatus::Error);
}

TEST(RespParse, BulkDataNotFollowedByCrlf) {
  EXPECT_EQ(parse("*1\r\n$3\r\nGETXX").status, ParseStatus::Error);
}

TEST(RespParse, LengthLineTooLongWithoutCrlf) {
  EXPECT_EQ(parse("*1\r\n$11111111111111111111111111").status, ParseStatus::Error);
}

TEST(RespParse, TooManyArguments) {
  EXPECT_EQ(parse("*17\r\n").status, ParseStatus::Error);
  std::string inline_cmd;
  for (int i = 0; i < 17; ++i) inline_cmd += "a ";
  EXPECT_EQ(parse(inline_cmd + "\n").status, ParseStatus::Error);
}

TEST(RespParse, BulkLongerThanRequestLimit) {
  EXPECT_EQ(parse("*1\r\n$2000\r\n").status, ParseStatus::Error);
}

TEST(RespParse, IncompleteRequestOverLimitIsError) {
  // A client streaming an endless line must not grow our buffer forever.
  EXPECT_EQ(parse(std::string(1024, 'a')).status, ParseStatus::Error);
}

// ---- Encoding replies ----------------------------------------------------------------

TEST(RespWrite, AllReplyTypes) {
  std::string out;
  write_simple(out, "OK");
  write_error(out, "ERR bad");
  write_integer(out, -3);
  write_bulk(out, "hi");
  write_null(out);
  write_array_header(out, 0);
  EXPECT_EQ(out, "+OK\r\n-ERR bad\r\n:-3\r\n$2\r\nhi\r\n$-1\r\n*0\r\n");
}

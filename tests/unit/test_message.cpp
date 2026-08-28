#include <sstream>

#include "common/message.h"
#include "test_framework.h"

using namespace p2p;

TEST(split_args_basics) {
  auto v = split_args("upload_file g1 file.bin");
  CHECK_EQ(v.size(), size_t(3));
  CHECK_EQ(v[0], std::string("upload_file"));
  CHECK_EQ(v[2], std::string("file.bin"));
}

TEST(split_args_collapses_runs_of_whitespace) {
  auto v = split_args("  a\t\tb   c  ");
  CHECK_EQ(v.size(), size_t(3));
  CHECK_EQ(v[0], std::string("a"));
  CHECK_EQ(v[2], std::string("c"));
}

TEST(split_args_empty_input) {
  CHECK(split_args("").empty());
  CHECK(split_args("    ").empty());
}

// parse_int/parse_ll replaced std::stoi specifically because stoi throws
// on malformed input and accepts trailing garbage. Both behaviours are
// pinned here.
TEST(parse_int_accepts_valid) {
  int v = 0;
  CHECK(parse_int("0", v));      CHECK_EQ(v, 0);
  CHECK(parse_int("42", v));     CHECK_EQ(v, 42);
  CHECK(parse_int("-7", v));     CHECK_EQ(v, -7);
  CHECK(parse_int("2147483647", v)); CHECK_EQ(v, 2147483647);
}

TEST(parse_int_rejects_malformed) {
  int v = 0;
  CHECK(!parse_int("", v));
  CHECK(!parse_int("abc", v));
  CHECK(!parse_int("12abc", v));   // stoi would have accepted this as 12
  CHECK(!parse_int("abc12", v));
  CHECK(!parse_int("1.5", v));
  CHECK(!parse_int(" 12", v));
  CHECK(!parse_int("12 ", v));
}

TEST(parse_int_rejects_overflow) {
  int v = 0;
  CHECK(!parse_int("2147483648", v));            // INT_MAX + 1
  CHECK(!parse_int("-2147483649", v));
  CHECK(!parse_int("99999999999999999999", v));
}

TEST(parse_ll_range) {
  long long v = 0;
  CHECK(parse_ll("9223372036854775807", v));
  CHECK_EQ(v, 9223372036854775807LL);
  CHECK(!parse_ll("9223372036854775808", v));
  CHECK(!parse_ll("nope", v));
}

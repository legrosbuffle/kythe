// Checks that the content of proto string literals is indexed.
#include "proto_parsetextproto.h"

class string;

// This mimicks a generated protobuf:
// message Inner {
//   optional int32 my_int = 1;
// }
// message Outer {
//   optional Inner inner = 1;
//   optional int my_int = 2;
// }
namespace some {
namespace package {

//- @Inner defines/binding InnerProto
class Inner {
 public:
  //- @my_int defines/binding MyIntAccessor
  //- MyIntAccessor childof InnerProto
  int my_int() const;
};

//- @Outer defines/binding OuterProto
class Outer {
 public:
  //- @inner defines/binding InnerAccessor
  //- InnerAccessor childof OuterProto
  const Inner& inner() const;
  //- @my_string defines/binding MyStringAccessor
  //- MyStringAccessor childof OuterProto
  const string& my_string() const;
};
}  // namespace package
}  // namespace some

int main() {
  //- @inner defines/binding InnerVar
  some::package::Inner inner;

  //- @inner ref InnerVar
  //- @my_int ref MyIntAccessor
  //- @"inner.my_int()" ref/call MyIntAccessor
  inner.my_int();

  const some::package::Outer msg =
      ::proto2::contrib::parse_proto::ParseTextProtoOrDieAt(
          //- LiteralInner.node/kind anchor
          //- LiteralInner.loc/start @^:64"in"
          //- LiteralInner.loc/end @$:64"ner"
          //- LiteralInner ref/call InnerAccessor

          //- LiteralMyInt.node/kind anchor
          //- LiteralMyInt.loc/start @^:65"my_"
          //- LiteralMyInt.loc/end @$:65"int"
          //- LiteralMyInt ref/call MyIntAccessor

          //- LiteralMyString.node/kind anchor
          //- LiteralMyString.loc/start @^:67"my_"
          //- LiteralMyString.loc/end @$:67"string"
          //- LiteralMyString ref/call MyStringAccessor

          " inner {"
          "  my_int: 3\n"
          " }"
          " my_string: 'blah'",
          false, __FILE__, __LINE__);
}

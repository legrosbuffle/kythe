// Constructors are indexed.
//- @f defines/binding FnF
void f() { }
//- @C defines/binding ClassC
class C {
 public:
  //- @C defines/binding CtorC
  //- CtorC childof ClassC
  //- CtorC.node/kind function
  //- CtorC.subkind constructor
  //- Call ref/call FnF
  //- Call childof CtorC
  C() { f(); }
  //- @C defines/binding CtorC2
  //- CtorC2 childof ClassC
  C(int) { }
};
//- @g defines/binding FnG
void g() {
  //- CallC ref/call CtorC
  //- CallC.loc/start @^"c"
  //- CallC.loc/end @^"c"
  C c;
  //- @"C(42)" ref/call CtorC2
  C(42);
}

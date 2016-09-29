// Checks that we index InitListExprs.
//- @C defines/binding ClassC
//- @f defines/binding FieldF
class C { int f; };

//- @"{0}" ref ClassC
//- @"{0}" typed ClassC
C c = {0};

package meta

func Foobar() {}
//   ^     ^ offset 25
//   \ offset 19

// Note: The locations in this file are connected to the offsets defined in the
// associated meta file. If you move anything above this comment without
// updating the metadata, the test may break.

//- FA.node/kind anchor
//- FA.loc/start 19
//- FA.loc/end   25
//- FA defines/binding Foobar
//- Foobar.node/kind function
//- Alt=vname(gsig, gcorp, groot, gpath, glang) generates Foobar

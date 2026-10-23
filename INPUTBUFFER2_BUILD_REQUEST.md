# InputBuffer2 isolated build

This branch exists only to trigger the isolated Windows build workflow at `.github/workflows/build-inputbuffer2-safe.yml`.

The workflow checks out the reviewed upstream source at commit `004bca30d08fd104f69261ccd655bc0a4baf0870`, assigns a unique local Geode mod ID (`local.inputbuffer2-safe`) and compiles a `.geode` artifact. No source from InputBuffer2 is copied into this repository branch; the workflow fetches it during the build.

The resulting mod is a test-only local build, not an official release and not the predictive LilWC mod.
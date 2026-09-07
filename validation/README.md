# Validation harnesses

From an x64 Visual Studio developer prompt in the archive root:

```
cl /nologo /std:c++17 /EHsc /utf-8 /I src validation/test_dynamic_light_policy.cpp /Fe:lighting_policy_test.exe
lighting_policy_test.exe
cl /nologo /std:c++17 /EHsc /utf-8 /I src validation/regression.cpp /Fe:hud_timer_test.exe
hud_timer_test.exe
```

The additional test_matcher.cpp and test_publication.cpp exercise the actual
production matcher/resolver. Compile with /I src, and for test_publication.cpp
also compile src/GameAddresses.cpp with /EHa. They take local code-section
RVA/file pairs (publication test also takes stock/missing/duplicate/moved/unreadable
as its first argument). Proprietary code captures are not included.

These policy/renderer-boundary checks do not execute the game's lighting
command queue or validate full-campaign behavior. Stock game captures are
intentionally excluded. Read MAINTENANCE_R5.md for scope and build details.

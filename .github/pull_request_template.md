## Summary

- 

## Verification

- [ ] `.\scripts\configure-build.ps1 -Build -Configuration RelWithDebInfo`
- [ ] `ctest --test-dir build -C RelWithDebInfo --output-on-failure`
- [ ] `.\scripts\build-installer.ps1` if installer or runtime staging changed
- [ ] Verified stock Apple / Windows USB behavior when OBS tethering is inactive (if USB tethering changed)

## Notes

- 

## Safety

- [ ] No secrets, tokens, private device serials, or large binary blobs are included.
- [ ] Legal, third-party license, and known-limitation docs remain accurate.

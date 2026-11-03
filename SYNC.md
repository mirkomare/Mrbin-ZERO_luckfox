# mrbin-core (Luckfox / RV1106)

Copia vendored dei sorgenti GUI/recorder usati per **`sdk/fast/build_core.sh`** e **`sdk/build_mrbin_core.sh`**.

Origine al sync del commit fast 0.7.3f: `Mrbin/LUCKFOX` (`CMakeLists.txt`, `toolchain-luckfox.cmake`, `src/`, `platform/`). Patch RV1106: `sdk/mrbin-core-rv1106.patch`. Gate camera: `sdk/fast/mrbin-core-gate.py`.

Per aggiornare da LUCKFOX (solo in questo repo):

```powershell
Copy-Item -Recurse -Force C:\CURSOR\Mrbin\LUCKFOX\src, C:\CURSOR\Mrbin\LUCKFOX\platform C:\CURSOR\Mrbin-rv1106\sdk\mrbin-core-luckfox\
Copy-Item -Force C:\CURSOR\Mrbin\LUCKFOX\CMakeLists.txt, C:\CURSOR\Mrbin\LUCKFOX\toolchain-luckfox.cmake C:\CURSOR\Mrbin-rv1106\sdk\mrbin-core-luckfox\
```

GUI badge PIR / lista `mrbin/` in web: previsto **0.7.3g** nel sorgente LUCKFOX upstream; il binario in `sdk/fast/prebuilt/mrbin-core` riflette lo snapshot al build 0.7.3f.

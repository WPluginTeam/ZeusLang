@echo off
set "ZEUS_RUNTIME_DEBUG_FLAGS="
if /i "%ZEUS_RUNTIME_DEBUG_DETAILS%"=="1" set "ZEUS_RUNTIME_DEBUG_FLAGS=/DZEUS_RUNTIME_DEBUG_DETAILS=1"
cl.exe /O2 /GL /MT /utf-8 /EHsc /nologo /std:c++17 %ZEUS_RUNTIME_DEBUG_FLAGS% /Fe"./Zeus.exe" "Zeus.cpp" "ZeusRuntimeLib\ZeusGCABI.cpp" "ZeusRuntimeLib\ZeusThreadABI.cpp" "ZeusRuntimeLib\ZeusSocketABI.cpp" "ZeusRuntimeLib\ZeusRuntimeDebug.cpp" "ZeusRuntimeLib\ZeusABI.cpp" "ZeusRuntimeLib\ZeusMemoryABI.cpp" "ZeusRuntimeLib\ZeusLValueABI.cpp" "ZeusRuntimeLib\ZeusDynamicABI.cpp" "ZeusRuntimeLib\ZeusDynamicBinaryABI.cpp" "ZeusRuntimeLib\ZeusDynamicMemberABI.cpp" "ZeusRuntimeLib\ZeusRangeCheckABI.cpp" "ZeusRuntimeLib\ZeusClosureABI.cpp" "ZeusRuntimeLib\ZeusObjectLifetime.cpp" "ZeusRuntimeLib\ZeusThreadClosureABI.cpp" /link /LTCG

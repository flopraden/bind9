# Microsoft Developer Studio Generated NMAKE File, Based on namedcheckconf.dsp
!IF "$(CFG)" == ""
CFG=namedcheckconf - Win32 Debug
!MESSAGE No configuration specified. Defaulting to namedcheckconf - Win32 Debug.
!ENDIF 

!IF "$(CFG)" != "namedcheckconf - Win32 Release" && "$(CFG)" != "namedcheckconf - Win32 Debug"
!MESSAGE Invalid configuration "$(CFG)" specified.
!MESSAGE You can specify a configuration when running NMAKE
!MESSAGE by defining the macro CFG on the command line. For example:
!MESSAGE 
!MESSAGE NMAKE /f "namedcheckconf.mak" CFG="namedcheckconf - Win32 Debug"
!MESSAGE 
!MESSAGE Possible choices for configuration are:
!MESSAGE 
!MESSAGE "namedcheckconf - Win32 Release" (based on "Win32 (x86) Console Application")
!MESSAGE "namedcheckconf - Win32 Debug" (based on "Win32 (x86) Console Application")
!MESSAGE 
!ERROR An invalid configuration is specified.
!ENDIF 

!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 

CPP=cl.exe
RSC=rc.exe

!IF  "$(CFG)" == "namedcheckconf - Win32 Release"

OUTDIR=.\Release
INTDIR=.\Release

!IF "$(RECURSE)" == "0" 

ALL : "..\..\..\Build\Release\named-checkconf.exe"

!ELSE 

ALL : "libisccfg - Win32 Release" "libisc - Win32 Release" "..\..\..\Build\Release\named-checkconf.exe"

!ENDIF 

!IF "$(RECURSE)" == "1" 
CLEAN :"libisc - Win32 ReleaseCLEAN" "libisccfg - Win32 ReleaseCLEAN" 
!ELSE 
CLEAN :
!ENDIF 
	-@erase "$(INTDIR)\check-tool.obj"
	-@erase "$(INTDIR)\named-checkconf.obj"
	-@erase "$(INTDIR)\vc60.idb"
	-@erase "..\..\..\Build\Release\named-checkconf.exe"

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP_PROJ=/nologo /MT /W3 /GX /O2 /I "./" /I "../../../" /I "../../../lib/isc/win32" /I "../../../lib/isc/win32/include" /I "../../../lib/isc/include" /I "../../../lib/isccfg/include" /D "NDEBUG" /D "WIN32" /D "_CONSOLE" /D "_MBCS" /D "__STDC__" /Fp"$(INTDIR)\namedcheckconf.pch" /YX /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /FD /c 
BSC32=bscmake.exe
BSC32_FLAGS=/nologo /o"$(OUTDIR)\namedcheckconf.bsc" 
BSC32_SBRS= \
	
LINK32=link.exe
LINK32_FLAGS=user32.lib advapi32.lib ws2_32.lib ../../../lib/isc/win32/Release/libisc.lib ../../../lib/isccfg/win32/Release/libisccfg.lib /nologo /subsystem:console /incremental:no /pdb:"$(OUTDIR)\named-checkconf.pdb" /machine:I386 /out:"../../../Build/Release/named-checkconf.exe" 
LINK32_OBJS= \
	"$(INTDIR)\check-tool.obj" \
	"$(INTDIR)\named-checkconf.obj" \
	"..\..\..\lib\isc\win32\Release\libisc.lib" \
	"..\..\..\lib\isccfg\win32\Release\libisccfg.lib"

"..\..\..\Build\Release\named-checkconf.exe" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

!ELSEIF  "$(CFG)" == "namedcheckconf - Win32 Debug"

OUTDIR=.\Debug
INTDIR=.\Debug
# Begin Custom Macros
OutDir=.\Debug
# End Custom Macros

!IF "$(RECURSE)" == "0" 

ALL : "..\..\..\Build\Debug\named-checkconf.exe" "$(OUTDIR)\namedcheckconf.bsc"

!ELSE 

ALL : "libisccfg - Win32 Debug" "libisc - Win32 Debug" "..\..\..\Build\Debug\named-checkconf.exe" "$(OUTDIR)\namedcheckconf.bsc"

!ENDIF 

!IF "$(RECURSE)" == "1" 
CLEAN :"libisc - Win32 DebugCLEAN" "libisccfg - Win32 DebugCLEAN" 
!ELSE 
CLEAN :
!ENDIF 
	-@erase "$(INTDIR)\check-tool.obj"
	-@erase "$(INTDIR)\check-tool.sbr"
	-@erase "$(INTDIR)\named-checkconf.obj"
	-@erase "$(INTDIR)\named-checkconf.sbr"
	-@erase "$(INTDIR)\vc60.idb"
	-@erase "$(INTDIR)\vc60.pdb"
	-@erase "$(OUTDIR)\named-checkconf.pdb"
	-@erase "$(OUTDIR)\namedcheckconf.bsc"
	-@erase "..\..\..\Build\Debug\named-checkconf.exe"
	-@erase "..\..\..\Build\Debug\named-checkconf.ilk"

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP_PROJ=/nologo /MTd /W3 /Gm /GX /ZI /Od /I "./" /I "../../../" /I "../../../lib/isc/win32" /I "../../../lib/isc/win32/include" /I "../../../lib/isc/include" /I "../../../lib/isccfg/include" /D "_DEBUG" /D "__STDC__" /D "WIN32" /D "_CONSOLE" /D "_MBCS" /FR"$(INTDIR)\\" /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /FD /GZ /c 
BSC32=bscmake.exe
BSC32_FLAGS=/nologo /o"$(OUTDIR)\namedcheckconf.bsc" 
BSC32_SBRS= \
	"$(INTDIR)\check-tool.sbr" \
	"$(INTDIR)\named-checkconf.sbr"

"$(OUTDIR)\namedcheckconf.bsc" : "$(OUTDIR)" $(BSC32_SBRS)
    $(BSC32) @<<
  $(BSC32_FLAGS) $(BSC32_SBRS)
<<

LINK32=link.exe
LINK32_FLAGS=user32.lib advapi32.lib ws2_32.lib ../../../lib/isc/win32/Debug/libisc.lib ../../../lib/isccfg/win32/Debug/libisccfg.lib /nologo /subsystem:console /incremental:yes /pdb:"$(OUTDIR)\named-checkconf.pdb" /debug /machine:I386 /out:"../../../Build/Debug/named-checkconf.exe" /pdbtype:sept 
LINK32_OBJS= \
	"$(INTDIR)\check-tool.obj" \
	"$(INTDIR)\named-checkconf.obj" \
	"..\..\..\lib\isc\win32\Debug\libisc.lib" \
	"..\..\..\lib\isccfg\win32\Debug\libisccfg.lib"

"..\..\..\Build\Debug\named-checkconf.exe" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

!ENDIF 

.c{$(INTDIR)}.obj::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

.cpp{$(INTDIR)}.obj::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

.cxx{$(INTDIR)}.obj::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

.c{$(INTDIR)}.sbr::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

.cpp{$(INTDIR)}.sbr::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<

.cxx{$(INTDIR)}.sbr::
   $(CPP) @<<
   $(CPP_PROJ) $< 
<<


!IF "$(NO_EXTERNAL_DEPS)" != "1"
!IF EXISTS("namedcheckconf.dep")
!INCLUDE "namedcheckconf.dep"
!ELSE 
!MESSAGE Warning: cannot find "namedcheckconf.dep"
!ENDIF 
!ENDIF 


!IF "$(CFG)" == "namedcheckconf - Win32 Release" || "$(CFG)" == "namedcheckconf - Win32 Debug"
SOURCE="..\check-tool.c"

!IF  "$(CFG)" == "namedcheckconf - Win32 Release"


"$(INTDIR)\check-tool.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


!ELSEIF  "$(CFG)" == "namedcheckconf - Win32 Debug"


"$(INTDIR)\check-tool.obj"	"$(INTDIR)\check-tool.sbr" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


!ENDIF 

SOURCE="..\named-checkconf.c"

!IF  "$(CFG)" == "namedcheckconf - Win32 Release"


"$(INTDIR)\named-checkconf.obj" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


!ELSEIF  "$(CFG)" == "namedcheckconf - Win32 Debug"


"$(INTDIR)\named-checkconf.obj"	"$(INTDIR)\named-checkconf.sbr" : $(SOURCE) "$(INTDIR)"
	$(CPP) $(CPP_PROJ) $(SOURCE)


!ENDIF 

!IF  "$(CFG)" == "namedcheckconf - Win32 Release"

"libisc - Win32 Release" : 
   cd "\bind-9.3.0a0\lib\isc\win32"
   $(MAKE) /$(MAKEFLAGS) /F ".\libisc.mak" CFG="libisc - Win32 Release" 
   cd "..\..\..\bin\check\win32"

"libisc - Win32 ReleaseCLEAN" : 
   cd "\bind-9.3.0a0\lib\isc\win32"
   $(MAKE) /$(MAKEFLAGS) /F ".\libisc.mak" CFG="libisc - Win32 Release" RECURSE=1 CLEAN 
   cd "..\..\..\bin\check\win32"

!ELSEIF  "$(CFG)" == "namedcheckconf - Win32 Debug"

"libisc - Win32 Debug" : 
   cd "\bind-9.3.0a0\lib\isc\win32"
   $(MAKE) /$(MAKEFLAGS) /F ".\libisc.mak" CFG="libisc - Win32 Debug" 
   cd "..\..\..\bin\check\win32"

"libisc - Win32 DebugCLEAN" : 
   cd "\bind-9.3.0a0\lib\isc\win32"
   $(MAKE) /$(MAKEFLAGS) /F ".\libisc.mak" CFG="libisc - Win32 Debug" RECURSE=1 CLEAN 
   cd "..\..\..\bin\check\win32"

!ENDIF 

!IF  "$(CFG)" == "namedcheckconf - Win32 Release"

"libisccfg - Win32 Release" : 
   cd "\bind-9.3.0a0\lib\isccfg\win32"
   $(MAKE) /$(MAKEFLAGS) /F ".\libisccfg.mak" CFG="libisccfg - Win32 Release" 
   cd "..\..\..\bin\check\win32"

"libisccfg - Win32 ReleaseCLEAN" : 
   cd "\bind-9.3.0a0\lib\isccfg\win32"
   $(MAKE) /$(MAKEFLAGS) /F ".\libisccfg.mak" CFG="libisccfg - Win32 Release" RECURSE=1 CLEAN 
   cd "..\..\..\bin\check\win32"

!ELSEIF  "$(CFG)" == "namedcheckconf - Win32 Debug"

"libisccfg - Win32 Debug" : 
   cd "\bind-9.3.0a0\lib\isccfg\win32"
   $(MAKE) /$(MAKEFLAGS) /F ".\libisccfg.mak" CFG="libisccfg - Win32 Debug" 
   cd "..\..\..\bin\check\win32"

"libisccfg - Win32 DebugCLEAN" : 
   cd "\bind-9.3.0a0\lib\isccfg\win32"
   $(MAKE) /$(MAKEFLAGS) /F ".\libisccfg.mak" CFG="libisccfg - Win32 Debug" RECURSE=1 CLEAN 
   cd "..\..\..\bin\check\win32"

!ENDIF 


!ENDIF 


@echo on

call mastering_build_data.bat no_pause

pushd ..\..

set destDir=..\_MASTER
set outputDir=%destDir%\sonic3air_game
set msbuildPath="C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe"



:: Preparations
rmdir "%destDir%" /s /q
mkdir "%outputDir%"


:: Make sure the needed binaries are all up-to-date

%msbuildPath% ..\oxygenengine\_vstudio\oxygenengine.sln /property:Configuration=Release /property:Platform=Win32 -verbosity:minimal
%msbuildPath% _vstudio\soncthrickles.sln /property:Configuration=Release-Enduser /property:Platform=Win32 -verbosity:minimal



:: Build the master installation

robocopy "_master_image_template" "%outputDir%" /e
robocopy "data\font" "%outputDir%\data\font" /e

copy "bin\Release-Enduser_x86\*.exe" "%outputDir%"
copy "source\external\discord_game_sdk\lib\x86\discord_game_sdk.dll" "%outputDir%"


:: Add Oxygen engine

robocopy "..\oxygenengine\_master_image_template" "%outputDir%\bonus\oxygenengine" /e
copy "..\oxygenengine\bin\Release_x86\OxygenApp.exe" "%outputDir%\bonus\oxygenengine\"
mkdir "%outputDir%\data"
robocopy "..\oxygenengine\data" "%outputDir%\bonus\oxygenengine\data" /e


:: Complete S3AIR dev

robocopy "scripts" "%outputDir%\bonus\sonic3air_dev\scripts" /e



:: Pack everything as a ZIP

pushd "%destDir%"
	"C:\Program Files\7-Zip\7z.exe" a -tzip -r sonic3air_game.zip %outputDir%
popd


:: Archive artifacts
mkdir "%destDir%\artifacts\bin"
copy "bin\Release-Enduser_x86\*.exe" "%destDir%\artifacts\bin"
copy "bin\Release-Enduser_x86\*.pdb" "%destDir%\artifacts\bin"

mkdir "%destDir%\artifacts\bin"
robocopy "scripts" "%destDir%\artifacts\scripts" /e

pushd "%destDir%"
	"C:\Program Files\7-Zip\7z.exe" a -t7z -mx1 -r artifacts.7z artifacts
popd

rmdir "%destDir%\artifacts" /s /q

popd


:: Done
pause

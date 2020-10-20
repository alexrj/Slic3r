# Written by Joseph Lenox 
# Licensed under the same license as the rest of Slic3r.
# ------------------------
# You need to have Strawberry Perl 5.24.0.1 (or slic3r-perl) installed for this to work, 
Param
(
    # Perl version major/minor number. Slic3r perl uses 524
    [string]$perlVersion = "524",
    # Override the output file name.
    [string]$outputFile = "",
    [string]$currentDate = "$(Get-Date -UFormat '%Y.%m.%d')",
    # Override the branch name used in the output. Otherwise autodetect based on git.
    [string]$branch = "",
    #This is "32bit" or "64bit". It will detect based on presence of libglut.
    [string]$arch = $env:ARCH,
    # Change this to where you have Strawberry Perl installed.
    [string]$STRAWBERRY_PATH = "C:\Strawberry",
    [switch]$skipInstallPAR
)

function Get-ScriptDirectory
{
  $Invocation = (Get-Variable MyInvocation -Scope 1).Value
  Split-Path $Invocation.MyCommand.Path
}
$scriptDir = Get-ScriptDirectory

echo "Make this is run from the perl command window." 
echo "Requires PAR."

$perldll = "perl$perlVersion"

if ($branch -eq "") {
    git branch | foreach {
        if ($env:APPVEYOR) {
            if ($_ -match "`  (.*)") {
                $branch += $matches[1]
            }
        } else {
            if ($_ -match "\*` (.*)"){
                $branch += $matches[1]
            }
        }
    }
}

if ($outputFile -eq "") {
    $outputFile = $output_zip
}

if (!($arch -eq "64bit" -Or $arch -eq "32bit")) {
    # detect current version through libglut
    if (Test-Path "${STRAWBERRY_PATH}\c\bin\libglut-0__.dll") {
        $arch = "64bit"
    } else {
        $arch = "32bit"
    }
    echo "Arch: $arch"
}

if ($env:APPVEYOR) {
    if ($env:APPVEYOR_PULL_REQUEST_NUMBER -eq "") {
        $output_zip = "${scriptDir}\..\..\Slic3r-${branch}.${currentDate}.${env:APPVEYOR_BUILD_NUMBER}.$(git rev-parse --short HEAD).${arch}.zip"
    } else {
        $output_zip = "${scriptDir}\..\..\Slic3r-${branch}.${currentDate}.${env:APPVEYOR_BUILD_NUMBER}.$(git rev-parse --short HEAD).${arch}-PR${APPVEYOR_PULL_REQUEST_NUMBER}.zip"
    }
} else {
    $output_zip = "${scriptDir}\..\..\Slic3r-${branch}.${currentDate}.$(git rev-parse --short HEAD).${arch}.zip"
}

if ($outputFile -eq "") {
    $outputFile = $output_zip
}

if (-Not $skipInstallPAR) {
    cpanm "PAR::Packer"
}

# Some file names change based on 64bit/32bit. Set them here.
if ($arch -eq "32bit") { 
	$perlarch = "sjlj"
	$glut = "libglut-0_.dll"
	$pthread= "pthreadGC2-w32.dll"
} else {
	$perlarch = "seh"
	$glut = "libglut-0__.dll"
	$pthread= "pthreadGC2-w64.dll"
}

if (!( (Test-Path -Path "${scriptDir}\slic3r.exe") -And (Test-Path -Path "${scriptDir}\slic3r-console.exe") -And (Test-Path -Path "${scriptDir}\slic3r-debug-console.exe") ) ) {
    echo "Compiling Slic3r binaries"
    & ${scriptDir}\compile_wrapper.ps1 -perlVersion=$perlVersion -STRAWBERRY_PATH=$STRAWBERRY_PATH
}

# remove all static libraries, they just take up space.
if ($env:APPVEYOR) {
    gci ${scriptDir}\..\..\ -recurse | ? {$_.Name -match ".*\.a$"} | ri
    gci -recurse ${scriptDir}\..\..\local-lib | ? {$_.PSIsContainer -And $_.Name -match "DocView|IPC|DataView|Media|Ribbon|Calendar|STC|PerlTest|WebView"} | ri
    gci -recurse ${scriptDir}\..\..\local-lib| ? {$_.Name -match ".*(webview|ribbon|stc).*\.dll"} | ri
    gci -recurse ${scriptDir}\..\..\local-lib| ? {$_.Name -match ".*(webview|ribbon|stc).*\.dll"} | ri
    gci -recurse ${scriptDir}\..\..\local-lib| ? {$_.Name -match "^ExtUtils$"} | ri
    gci -recurse ${scriptDir}\..\..\local-lib\lib\perl5\Module ? {$_.Name -match "^Build"} | ri
    gci -recurse ${scriptDir}\..\..\local-lib ? {$_.Name -match "\.pod$"} | ri
    gci -recurse ${scriptDir}\..\..\local-lib ? {$_.Name -match "\.h$"} | ri
}

pp `
-a "${scriptDir}/slic3r.exe;Slic3r.exe"  `
-a "${scriptDir}/slic3r-console.exe;Slic3r-console.exe"  `
-a "${scriptDir}/slic3r-debug-console.exe;Slic3r-debug-console.exe"  `
-a "${scriptDir}/../../lib;lib" `
-a "${scriptDir}/../../local-lib;local-lib" `
-a "${scriptDir}/../../slic3r.pl;slic3r.pl" `
-a "${scriptDir}/../../var;var"  `
-a "${scriptDir}/../../FreeGLUT/freeglut.dll;freeglut.dll" `
-a "${STRAWBERRY_PATH}\perl\bin\perl${perlVersion}.dll;perl${perlVersion}.dll"  `
-a "${STRAWBERRY_PATH}\perl\bin\libstdc++-6.dll;libstdc++-6.dll"  `
-a "${STRAWBERRY_PATH}\perl\bin\libgcc_s_${perlarch}-1.dll;libgcc_s_${perlarch}-1.dll"  `
-a "${STRAWBERRY_PATH}\perl\bin\libwinpthread-1.dll;libwinpthread-1.dll"  `
-a "${STRAWBERRY_PATH}\c\bin\${pthread};${pthread}"  `
-a "${STRAWBERRY_PATH}\c\bin\${glut};${glut}"  `
-M AutoLoader `
-M B `
-M Bundle::LWP `
-M Carp `
-M Class::Accessor `
-M Config `
-M Crypt::CBC `
-M Cwd `
-M Devel::GlobalDestruction `
-M Digest `
-M Digest::MD5 `
-M Digest::SHA `
-M Digest::base `
-M DynaLoader `
-M Errno `
-M Exporter `
-M Exporter::Heavy `
-M Fcntl `
-M File::Basename `
-M File::Glob `
-M File::Spec `
-M File::Spec::Unix `
-M File::Spec::Win32 `
-M FindBin `
-M HTTP::Config `
-M HTTP::Date `
-M HTTP::Headers `
-M HTTP::Headers::Util `
-M HTTP::Message `
-M HTTP::Request `
-M HTTP::Request::Common `
-M HTTP::Response `
-M HTTP::Status `
-M IO `
-M IO::Handle `
-M IO::Select `
-M IO::Socket `
-M LWP `
-M LWP::MediaTypes `
-M LWP::MemberMixin `
-M LWP::Protocol `
-M LWP::Protocol::http `
-M LWP::UserAgent `
-M Net::SSL `
-M List::Util `
-M Math::Trig `
-M Method::Generate::Accessor `
-M Method::Generate::BuildAll `
-M Method::Generate::Constructor `
-M Module::Runtime `
-M POSIX `
-M Pod::Escapes `
-M Pod::Text `
-M Pod::Usage `
-M SelectSaver `
-M Socket `
-M Socket6 `
-M Storable `
-M Sub::Exporter `
-M Sub::Exporter::Progressive `
-M Symbol `
-M Term::Cap `
-M Text::ParseWords `
-M Thread `
-M Thread::Queue `
-M Thread::Semaphore `
-M Tie::Handle `
-M Tie::Hash `
-M Tie::StdHandle `
-M Time::Local `
-M URI `
-M URI::Escape `
-M URI::http `
-M Unicode::Normalize `
-M Win32 `
-M Win32::API `
-M Win32::TieRegistry `
-M Win32::WinError `
-M Win32API::Registry `
-M XSLoader `
-B `
-M lib `
-p ${scriptDir}\..\..\slic3r.pl -o ${scriptDir}\..\..\slic3r.par


# switch renaming based on whether or not using packaged exe or zip 
# make this more useful for not being on the appveyor server
copy ${scriptDir}\..\..\slic3r.par ${outputFile}
echo "Package saved as ${outputFile}"
del ${scriptDir}\..\..\slic3r.par

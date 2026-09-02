[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
            "${ProjectRoot}/release-portable/${ProductName}-*-windows-*.zip"
        )
    }

    Remove-Item @RemoveArgs

    Log-Group "Archiving ${ProductName}..."
    $CompressArgs = @{
        Path = (Get-ChildItem -Path "${ProjectRoot}/release/${Configuration}" -Exclude "${OutputName}*.*")
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Log-Group

    Log-Group "Archiving portable ${ProductName}..."

    $InstallRoot = "${ProjectRoot}/release/${Configuration}/${ProductName}"
    $PortableRoot = "${ProjectRoot}/release-portable/${Configuration}"

    if ( ! ( Test-Path -Path "${InstallRoot}/bin" ) ) {
        throw "No packaged binaries found at ${InstallRoot}/bin"
    }

    if ( Test-Path -Path $PortableRoot ) {
        Remove-Item -Path $PortableRoot -Recurse -Force
    }

    # Portable OBS installations expect plugins in obs-plugins/64bit and their
    # resources in data/obs-plugins/<plugin name>, so the packaged install tree
    # is rearranged into that layout before it is archived.
    $PortableBinaryDir = "${PortableRoot}/obs-plugins"
    $PortableDataDir = "${PortableRoot}/data/obs-plugins/${ProductName}"

    New-Item -ItemType Directory -Force -Path $PortableBinaryDir, $PortableDataDir | Out-Null

    Copy-Item -Path "${InstallRoot}/bin/*" -Destination $PortableBinaryDir -Recurse -Force

    if ( Test-Path -Path "${InstallRoot}/data" ) {
        Copy-Item -Path "${InstallRoot}/data/*" -Destination $PortableDataDir -Recurse -Force
    }

    $CompressArgs = @{
        Path = (Get-ChildItem -Path $PortableRoot)
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release-portable/${OutputName}-Portable.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Log-Group
}

Package

# Obtain the unchanged packaged codec tools for the format-matrix test.
param(
    [string]$ManifestPath = (Join-Path $PSScriptRoot '..\latest-win.json'),
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [string]$ArchivePath = ''
)
$ErrorActionPreference='Stop'
$manifest=Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
if($manifest.sha256 -notmatch '^[a-fA-F0-9]{64}$'){throw 'Missing official package digest'}
$uri=[uri]$manifest.download_url
if($uri.Scheme -ne 'https'){throw 'Package URL must use HTTPS'}
if(-not $ArchivePath){
    $scratch=Join-Path ([IO.Path]::GetTempPath()) ('rlplayer-codecs-'+[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $scratch | Out-Null
    $ArchivePath=Join-Path $scratch 'official.zip'
    Invoke-WebRequest -UseBasicParsing -Uri $uri.AbsoluteUri -OutFile $ArchivePath
}
$actual=(Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash
if($actual -ne $manifest.sha256){throw 'Official package SHA256 mismatch'}
if(Test-Path -LiteralPath $OutputDir){throw 'Codec output must be a new isolated directory'}
New-Item -ItemType Directory -Path $OutputDir | Out-Null
$destination=(Get-Item -LiteralPath $OutputDir).FullName
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip=[IO.Compression.ZipFile]::OpenRead($ArchivePath)
try {
    foreach($name in @('ffmpeg.exe','ffprobe.exe')){
        $entries=@($zip.Entries | Where-Object {$_.Name -eq $name})
        if($entries.Count -ne 1){throw "Expected exactly one packaged $name"}
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entries[0],(Join-Path $destination $name),$false)
    }
} finally {$zip.Dispose()}
[pscustomobject]@{Source=$uri.AbsoluteUri;ArchiveSHA256=$actual;Directory=$destination} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination 'receipt.json') -Encoding UTF8
if($env:GITHUB_ENV){"RTA_FFMPEG_LITE_DIR=$destination"|Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8}
Write-Output "Verified packaged codec tools: $destination"

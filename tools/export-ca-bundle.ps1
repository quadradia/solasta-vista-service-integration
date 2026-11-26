<#
.SYNOPSIS
  Builds ca-bundle.pem for vista-sync.exe from this PC's Windows trusted root store.

.DESCRIPTION
  XP's own root store is years out of date and mbedTLS does not use it anyway, so the
  service trusts only the PEM file next to it. This exports a short list of widely used
  public root CAs (Let's Encrypt, DigiCert, Sectigo/USERTrust, GlobalSign, Google Trust
  Services, Amazon, Microsoft). Nothing is downloaded and no server is contacted.

  api.mysolasta.com chains to GlobalSign ECC Root CA - R4 and identity.nuvol9.com to ISRG Root X2
  (checked 2026-11-26). Windows only puts a root in its store the first time it builds a chain to it,
  so if one is reported missing, browse to that host once on this PC and run the export again.

  Once the real chains of the token host and the API host are confirmed, trim the bundle
  to just their roots.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\export-ca-bundle.ps1 -OutFile dist\SolastaVistaSync\ca-bundle.pem
#>
param([Parameter(Mandatory = $true)][string]$OutFile)

$wanted = @(
    'CN=ISRG Root X1, O=Internet Security Research Group, C=US',
    'CN=ISRG Root X2, O=Internet Security Research Group, C=US',
    'CN=DigiCert Global Root CA, OU=www.digicert.com, O=DigiCert Inc, C=US',
    'CN=DigiCert Global Root G2, OU=www.digicert.com, O=DigiCert Inc, C=US',
    'CN=DigiCert Global Root G3, OU=www.digicert.com, O=DigiCert Inc, C=US',
    'CN=DigiCert High Assurance EV Root CA, OU=www.digicert.com, O=DigiCert Inc, C=US',
    'CN=Baltimore CyberTrust Root, OU=CyberTrust, O=Baltimore, C=IE',
    'CN=USERTrust RSA Certification Authority, O=The USERTRUST Network, L=Jersey City, S=New Jersey, C=US',
    'CN=USERTrust ECC Certification Authority, O=The USERTRUST Network, L=Jersey City, S=New Jersey, C=US',
    'CN=GlobalSign, O=GlobalSign, OU=GlobalSign Root CA - R3',
    'CN=GlobalSign Root CA, OU=Root CA, O=GlobalSign nv-sa, C=BE',
    'CN=GlobalSign, O=GlobalSign, OU=GlobalSign ECC Root CA - R4',
    'CN=GTS Root R1, O=Google Trust Services LLC, C=US',
    'CN=GTS Root R4, O=Google Trust Services LLC, C=US',
    'CN=Amazon Root CA 1, O=Amazon, C=US',
    'CN=Microsoft RSA Root Certificate Authority 2017, O=Microsoft Corporation, C=US',
    'CN=Microsoft ECC Root Certificate Authority 2017, O=Microsoft Corporation, C=US',
    'CN=Go Daddy Root Certificate Authority - G2, O="GoDaddy.com, Inc.", L=Scottsdale, S=Arizona, C=US'
)

$now = Get-Date
$certs = Get-ChildItem Cert:\LocalMachine\Root | Where-Object { $wanted -contains $_.Subject -and $_.NotAfter -gt $now } |
    Sort-Object Subject -Unique
$sb = New-Object System.Text.StringBuilder
foreach ($c in $certs) {
    [void]$sb.AppendLine("# $($c.Subject)")
    [void]$sb.AppendLine("# SHA1 $($c.Thumbprint)  valid until $($c.NotAfter.ToString('yyyy-MM-dd'))")
    [void]$sb.AppendLine('-----BEGIN CERTIFICATE-----')
    [void]$sb.AppendLine([Convert]::ToBase64String($c.RawData, 'InsertLineBreaks'))
    [void]$sb.AppendLine('-----END CERTIFICATE-----')
}
$dir = Split-Path -Parent $OutFile
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
[System.IO.File]::WriteAllText($OutFile, $sb.ToString().Replace("`r`n", "`n"), (New-Object System.Text.UTF8Encoding($false)))
$missing = $wanted | Where-Object { $s = $_; -not ($certs | Where-Object { $_.Subject -eq $s }) }
Write-Output "Wrote $($certs.Count) root certificates to $OutFile"
foreach ($m in $missing) { Write-Output "  not in this PC's root store (skipped): $m" }

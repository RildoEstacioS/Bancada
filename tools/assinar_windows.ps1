<#
Assina o bancada.exe para o Windows confiar nele.

O Controle Inteligente de Aplicativos bloqueia todo programa sem assinatura que a Microsoft ainda não
conhece, e o SmartScreen avisa ao abrir um programa baixado. Os dois aceitam um programa assinado com
um certificado de assinatura de código emitido por uma autoridade que a Microsoft reconhece.
Certificado feito em casa (autoassinado) não serve: o Windows o ignora.

Só conferir se o arquivo já está assinado:
    powershell -ExecutionPolicy Bypass -File tools\assinar_windows.ps1 -Conferir

Com um certificado instalado no Windows (cartão, token USB ou na nuvem, como o SimplySign da Certum),
passando a impressão digital (SHA-1) dele, que aparece em certmgr.msc, aba Detalhes:
    powershell -ExecutionPolicy Bypass -File tools\assinar_windows.ps1 -Impressao 0123ABCD...

Com o Artifact Signing da Microsoft (Azure), passando o arquivo de metadados da conta e a DLL do cliente:
    powershell -ExecutionPolicy Bypass -File tools\assinar_windows.ps1 -Metadados metadata.json -Dlib Azure.CodeSigning.Dlib.dll

Precisa do signtool.exe, que vem no Windows SDK (basta o componente "Windows SDK Signing Tools for
Desktop Apps"). Conferir não precisa dele.
#>
param(
    [string]$Arquivo = "build\win\bancada.exe",
    [string]$Impressao,
    [string]$Metadados,
    [string]$Dlib,
    [string]$Carimbo,
    [switch]$Conferir
)

$ErrorActionPreference = "Stop"

function Mostrar-Assinatura([string]$caminho) {
    $assinatura = Get-AuthenticodeSignature -FilePath $caminho
    switch ($assinatura.Status) {
        "Valid" {
            Write-Host "Assinado e válido."
            Write-Host "  Quem assinou: $($assinatura.SignerCertificate.Subject)"
            Write-Host "  Emitido por:  $($assinatura.SignerCertificate.Issuer)"
            if ($assinatura.TimeStamperCertificate) {
                Write-Host "  Carimbo de tempo: sim (a assinatura continua valendo depois que o certificado vencer)"
            } else {
                Write-Host "  Carimbo de tempo: não (a assinatura deixa de valer quando o certificado vencer)"
            }
            return $true
        }
        "NotSigned" { Write-Host "Sem assinatura. O Controle Inteligente de Aplicativos vai bloquear este arquivo." }
        default { Write-Host "Assinatura com problema: $($assinatura.Status). $($assinatura.StatusMessage)" }
    }
    return $false
}

function Achar-Signtool {
    $noCaminho = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($noCaminho) { return $noCaminho.Source }
    $kits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    if (Test-Path $kits) {
        $achados = Get-ChildItem $kits -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "\\x64\\" } | Sort-Object FullName -Descending
        if ($achados) { return $achados[0].FullName }
    }
    return $null
}

if (-not (Test-Path $Arquivo)) {
    Write-Host "Não achei $Arquivo. Compile o jogo primeiro."
    exit 1
}
$Arquivo = (Resolve-Path $Arquivo).Path

if ($Conferir) {
    if (Mostrar-Assinatura $Arquivo) { exit 0 } else { exit 1 }
}

if ((-not $Impressao) -and (-not $Metadados)) {
    Write-Host "Diga qual certificado usar: -Impressao <SHA-1 do certificado> ou -Metadados <metadata.json> -Dlib <dll>."
    Write-Host "Para só ver como o arquivo está, use -Conferir."
    exit 2
}

$signtool = Achar-Signtool
if (-not $signtool) {
    Write-Host "Não achei o signtool.exe. Ele vem no Windows SDK:"
    Write-Host "  https://developer.microsoft.com/windows/downloads/windows-sdk/"
    Write-Host "Na instalação, basta marcar 'Windows SDK Signing Tools for Desktop Apps'."
    exit 3
}

$argumentos = @("sign", "/v", "/fd", "SHA256", "/td", "SHA256", "/d", "Bancada")
if ($Metadados) {
    if (-not $Dlib) {
        Write-Host "Com -Metadados, passe também -Dlib com o caminho da Azure.CodeSigning.Dlib.dll."
        exit 2
    }
    if (-not $Carimbo) { $Carimbo = "http://timestamp.acs.microsoft.com" }
    $argumentos += @("/tr", $Carimbo, "/dlib", $Dlib, "/dmdf", $Metadados)
} else {
    if (-not $Carimbo) { $Carimbo = "http://timestamp.digicert.com" }
    $argumentos += @("/tr", $Carimbo, "/sha1", $Impressao)
}
$argumentos += $Arquivo

Write-Host "Assinando $Arquivo"
& $signtool @argumentos
if ($LASTEXITCODE -ne 0) {
    Write-Host "O signtool falhou (código $LASTEXITCODE). Nada foi assinado."
    exit 4
}
& $signtool verify /pa $Arquivo | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Assinou, mas o Windows não aceita a assinatura. Confira se o certificado é de assinatura de código."
    exit 5
}
if (Mostrar-Assinatura $Arquivo) { exit 0 } else { exit 5 }

param (
    [switch]$Build,
    [switch]$Test
)

$ImageName = "autismlang-env"

if ($Build -or !(docker images -q $ImageName)) {
    Write-Host "Building Docker Image: $ImageName" -ForegroundColor Cyan
    docker build -t $ImageName .
}

if ($Test) {
    Write-Host "Running comprehensive tests inside Docker..." -ForegroundColor Green
    docker run --rm -v "${PWD}:/app" -w /app $ImageName make test
} else {
    Write-Host "Entering AutismLang Interactive Docker Shell..." -ForegroundColor Yellow
    Write-Host "You can type 'make test' inside to verify."
    docker run -it --rm -v "${PWD}:/app" -w /app $ImageName
}

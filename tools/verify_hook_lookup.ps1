[CmdletBinding()]
param(
    [string] $GameDirectory = 'E:\SteamLibrary\steamapps\common\In Falsus'
)

$ErrorActionPreference = 'Stop'
$dummyDirectory = Join-Path $GameDirectory 'MelonLoader\Dependencies\Il2CppAssemblyGenerator\Cpp2IL\cpp2il_out'
Add-Type -Path (Join-Path $GameDirectory 'MelonLoader\net472\Mono.Cecil.dll')

function Find-UniqueInstanceMethod {
    param($Owner, [string] $Name, [int] $GenericParameterCount, [string[]] $ParameterTypes)

    $matches = @($Owner.Methods | Where-Object {
        if ($_.Name -ne $Name -or $_.IsStatic -or
            $_.GenericParameters.Count -ne $GenericParameterCount -or
            $_.Parameters.Count -ne $ParameterTypes.Count) {
            return $false
        }
        for ($index = 0; $index -lt $ParameterTypes.Count; $index++) {
            if ($_.Parameters[$index].ParameterType.FullName -ne $ParameterTypes[$index]) {
                return $false
            }
        }
        return $true
    })
    if ($matches.Count -ne 1) {
        throw "Expected exactly one $Name overload; found $($matches.Count)."
    }
    return $matches[0]
}

$addressables = [Mono.Cecil.AssemblyDefinition]::ReadAssembly((Join-Path $dummyDirectory 'Unity.Addressables.dll'))
$resources = [Mono.Cecil.AssemblyDefinition]::ReadAssembly((Join-Path $dummyDirectory 'Unity.ResourceManager.dll'))
try {
    $owner = $addressables.MainModule.Types | Where-Object FullName -eq 'UnityEngine.AddressableAssets.AddressablesImpl'
    $loader = Find-UniqueInstanceMethod $owner 'LoadAssetAsync' 1 @('System.Object')
    if ($loader.ReturnType.FullName -ne 'UnityEngine.ResourceManagement.AsyncOperations.AsyncOperationHandle`1<TObject>') {
        throw 'The loader return type is incompatible.'
    }
    $owner = $resources.MainModule.Types | Where-Object FullName -eq 'UnityEngine.ResourceManagement.AsyncOperations.AsyncOperationHandle'
    $acquire = Find-UniqueInstanceMethod $owner 'Acquire' 0 @()
    if ($acquire.ReturnType.FullName -ne $owner.FullName) {
        throw 'The Acquire return type is incompatible.'
    }
    $loader.FullName
    $acquire.FullName
    'Current generated metadata signature checks passed (offline; not an in-game test).'
} finally {
    $addressables.Dispose()
    $resources.Dispose()
}

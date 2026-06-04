# Profiles & RNG

Same on Linux and Windows.

## Profiles

| Profile | Passes | Behavior |
|---------|--------|----------|
| ghost | 1 | Random overwrite (default) |
| chameleon | 1 | ghost + `--nonce` |
| spectrum | 2 | Two random passes |
| flash-realist | 1* | SSD erase first with `--ssd-secure-erase` |
| filesystem-shadow | 1 | Wipe, rename, delete |
| block-cartographer | 1 | Partition-table gaps |
| slack-hunter | 1 | Extend file to cluster, wipe |

\* Erase OK → skip overwrite.

## RNG

| Mode | Engine | Speed |
|------|--------|-------|
| vault | ChaCha20 | Fast (default) |
| turbo | xoshiro256** | Fastest |
| hybrid | ChaCha + xoshiro | Fast |
| os-chunk | OS CSPRNG/chunk | Slow |

## SSD erase (`--ssd-secure-erase`)

| Order | Linux | Windows |
|-------|-------|---------|
| 1 | NVMe Format NVM | NVMe pass-through |
| 2 | ATA SECURITY ERASE | - |
| 3 | BLKDISCARD | FSCTL_TRIM |

## Limits

- HPA/DCO: warning only; wipes visible size  
- ATA erase: often locked/rejected  
- SSD retired blocks: overwrite may not reach  
- TRIM: opt-in  
- Does not wipe shell history, logs, etc.

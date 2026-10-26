# Optional fonts

ClaudeUI builds and runs without any files here. The fonts it can use from
this folder belong to Anthropic and can't be redistributed, so they aren't
in the repository (`.gitignore` keeps them out).

If you have them, drop them in this folder. They're picked up at startup:

| File                        | Used for                           | Without it                                   |
|-----------------------------|------------------------------------|----------------------------------------------|
| `anthropic-sans.ttf`        | interface text (Latin)             | Noto Sans                                    |
| `anthropic-serif.ttf`       | the "ClaudeUI" wordmark            | Noto Serif                                   |
| `anthropic-mono.ttf`        | optional terminal font in Settings | the option isn't shown                       |
| `Anthropicons-Variable.ttf` | icons                              | Codicons from JetBrains Mono Nerd Font, or no icons |

System fonts the app expects (Arch packages): `noto-fonts` (Cyrillic, the
fallback interface font) and `ttf-jetbrains-mono-nerd` (terminal, icons).

`$CLAUDEUI_ASSETS` points the app at another assets folder.

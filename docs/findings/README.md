# Findings

Reverse-engineering notes about *Crash: Mind over Mutant* and the port,
one file per investigation, in the order they happened.

**About these notes:** like the rest of the project, they were written by an
AI coding assistant working with the project lead (see
[How this project is made](../../README.md#how-this-project-is-made-please-read)).
Every conclusion was tested against the running game (logs, screenshots,
debugger sessions, measurements), and wrong turns are kept in the text
because they're part of how the answer was found. They have not been
reviewed by a human expert yet: corrections and reviews are very welcome
and will be credited.

| # | Topic |
|---|---|
| [01](01-disc-and-executable.md) | The disc, the archives and the executable |
| [02](02-recompilation.md) | Getting the recompiler to translate the whole game |
| [03](03-first-boot.md) | First boot |
| [04](04-loading-hang-xma.md) | The loading-screen hang: an audio decoder bug in the SDK |
| [05](05-post-intro-freeze-fibers.md) | The freeze after the intro movies: a fiber bug in the SDK |
| [06](06-black-screens-render-target-path.md) | Black movies and the vanishing title logo: the render-target path |
| [07](07-frame-rate.md) | The 30 fps cap and the 60 fps option |

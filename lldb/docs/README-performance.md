# Why can opening a debugger be slow?

This is a beginner-friendly guide to the changes in this draft. Start with the
library analogy, then follow the code links when you want more detail.

The app is a huge library. Its frameworks are books. The debugger is a librarian
trying to answer a small question: **“Where did this program stop, and what is
this variable?”**

A slow answer can come from cataloguing every book before answering. The aim of
this work is to inspect enough of the catalogue to answer correctly, and leave
unrelated work until it is actually needed.

## 1. What happens when you start debugging?

Consider typing `expression items.count` while stopped in a Swift function.
Several different systems help answer that question:

1. **Choose a target.** LLDB learns which executable and architecture it will
   debug. A target is its description of the program; a process is one running
   instance of that program.
2. **Attach to the process.** LLDB connects to the running app through a helper,
   usually `debugserver` on Apple platforms. The helper can read memory, control
   execution and report stops.
3. **Discover loaded images.** An image here is an executable or shared library,
   not a picture. LLDB needs to know which code is loaded and where.
4. **Find source and variables.** Debug information connects machine addresses
   to source lines, variable names and types. A breakpoint is a requested place
   to stop; resolving it means finding its actual machine-code location.
5. **Understand Swift and its dependencies.** The expression compiler needs
   types, imported modules and sometimes C or Objective-C declarations.
6. **Compile and run the expression.** LLDB can generate a small piece of machine
   code, connect it to the app's functions and data, and execute it.

These are different stages. Making attach faster does not necessarily make the
first expression faster: some work may simply happen later.

## 2. The small vocabulary that makes the rest easier

| Term | Plain-English meaning |
| --- | --- |
| LLDB | The debugger: the tool that lets you stop a program and inspect it. |
| Framework / dynamic library | A separately built collection of code that the app loads. |
| Module | In LLDB's image list, a representation of an executable or library. In the compiler, a collection of declarations such as a Swift or Clang module. These are related ideas, not interchangeable objects. |
| Mach-O | The executable-file format used by these Apple-platform code images. |
| Symbol | A named entry describing something such as a function, variable or reference. |
| Symbol table / `Symtab` | An address book of symbols, with information about their kind and location. |
| Metadata | Information describing code or data, rather than the running application logic itself. |
| DWARF | Debug information that describes source lines, types, variables and more. |
| dSYM | A separate debug-information bundle. It must match the binary being debugged. |
| Debug map | Directions from a linked binary to object files containing its debug information. |
| AST | Abstract syntax tree: the compiler's structured understanding of source code. Serialized Swift modules can contain this information. |
| JIT | Just-in-time compilation: producing executable code while the debugger is running. |
| Reflection | Learning about runtime types and values from their metadata. |
| ABI | The agreement compiled code follows about names, calling conventions and data layout. |
| Target triple | A compiler identifier describing an architecture, operating system and related environment. |
| UUID | A binary identifier used to help match the correct image and debug information. A matching filename alone is insufficient. |
| Cache | Previously computed information retained for reuse under specific validity rules. |
| Lock / mutex | A rule that lets only an allowed set of operations use shared state at once. |

## 3. Problem: building the whole address book for one question

Imagine asking whether a book mentions `print`. The librarian first translates
and indexes every name in every book. That preparation may help later questions,
but it is expensive if most books cannot contain the requested name.

LLDB has full symbol tables and name indexes. Building them can involve reading
many records, decoding names and making extra lookup structures.

**This draft's change:** let supported object files answer a cheaper question:
“Can this exact kind of name possibly be here?” A complete, validated negative
answer lets the caller skip materializing that image's symbol table. A possible
match still goes through the appropriate real lookup.

This is not guessing that a name is absent. Unsupported formats, incomplete
data, malformed records and symbol providers that can add information keep the
normal path. The shortcut must not hide information supplied by DWARF or another
debug-information provider.

Two details matter:

- **Mangled names** are compiler-generated spellings that distinguish things
  like namespaces, types and overloaded functions. **Demangling** turns them
  into human-readable descriptions. A runtime query for an exact linker name
  should not need every unrelated name translated first.
- **C linkage** describes an ABI naming contract. It does not mean the whole
  application is written in C. Swift and C++ runtimes also expose entry points
  with C linkage.

Code: [object-file contract](../include/lldb/Symbol/ObjectFile.h),
[Mach-O implementation](../source/Plugins/ObjectFile/Mach-O/ObjectFileMachO.cpp),
[symbol lookup](../source/Symbol/Symtab.cpp).

## 4. Problem: a reference is not the thing it points to

An address book might say “ask the next building for Alice.” That is different
from saying “Alice lives here.” Treating both entries alike creates unnecessary
searches and can choose the wrong answer.

Compiled images contain several kinds of entries:

- A **defined symbol** supplies something here. An **undefined symbol** refers
  to something supplied elsewhere.
- A **trampoline or stub** is forwarding code, not necessarily the final
  implementation. Some records can become meaningful stubs during full parsing.
- A **resolver** chooses an implementation. A **re-export** points through
  another library. Both can matter to finding a function.
- A **weak symbol** has special optional or replacement behavior; finding one
  does not automatically settle the lookup.

**This draft's change:** distinguish general-name, defined-name and
function-name queries. A table containing only irrelevant references can then
be skipped for a definition or function lookup, while preserving the stubs,
resolvers and other cases that the particular caller requires.

Swift's memory reader first tries a unique, usable definition for a runtime
name. If it finds none, several candidates or an unusable address, it restores
the broader lookup and its existing handling.

The JIT also has a narrow shortcut when its own intermediate representation
already proves that a request is an ordinary external data declaration. It
accepts a unique suitable data symbol in the current image. It does not apply
that shortcut to every similarly spelled function, alias or thread-local value
(a value with a separate instance for each thread).

The important rule is **“use a stronger fact when available; otherwise use the
old resolution rules,”** not “take the first name that looks right.”

Code: [query kinds](../include/lldb/lldb-private-enumerations.h),
[Swift memory reader](../source/Plugins/LanguageRuntime/Swift/LLDBMemoryReader.cpp),
[JIT resolution](../source/Expression/IRExecutionUnit.cpp),
[linkage regression tests](../test/API/commands/expression/linkage_lookup/TestIRExecutionUnitLinkageLookup.py).

## 5. Problem: repeatedly reading the catalogue

Mach-O stores symbol records called **nlist entries**, a string table containing
names, and sometimes an **export trie** describing exported names. A trie is
like a directory organised by name prefixes: names with the same beginning
share a branch.

Even a cheaper raw-name lookup can become expensive if it repeatedly walks
every branch and decodes every unrelated record.

**This draft's change:** use bounded readers and narrowly scoped name data.
Supported C-name information can be retained with the object file. For supported
modern Swift linkage queries, a validated export trie can be searched along
branches capable of matching the requested spelling, rather than visiting all
exported spellings on every query.

There is a safety cost: a partial walk cannot prove that the whole structure is
valid. Complete validation and relevant resolver information must be established
before the directed shortcut can justify skipping work. Unsupported or
uncertain cases remain conservative.

This is also why “just add a global cache” is not a complete solution. A cache
must specify what owns its data, what makes it valid and which changes require
the caller to stop trusting it. Later symbol materialization or augmentation
must not be hidden by an earlier negative answer.

Code: [bounded symbol readers and export queries](../source/Plugins/ObjectFile/Mach-O/ObjectFileMachO.cpp),
[object-owned cache fields](../source/Plugins/ObjectFile/Mach-O/ObjectFileMachO.h),
[malformed-input and symbol-kind tests](../unittests/Symbol/SymtabTest.cpp).

## 6. Problem: asking for directions opens every book

Sometimes the debugger only needs the locations of Swift module data or the
object files named in a debug map. Loading the complete symbol table to obtain
those directions is like opening every book just to find its catalogue card.

**This draft's change:** expose selected format-owned debug metadata separately
from a fully populated symbol table. Debug-map and Swift AST consumers can use
that metadata when it is complete and appropriate, and retain full parsing as
the fallback.

The symbol-file plugin chooser also needs care. A **plugin** is an
implementation that knows how to handle a particular kind of information. Merely
asking “can this plugin help?” should not always force all of its expensive
indexes to be built. A narrowly proven case defers those indexes until an
operation actually needs them.

**Lazy** means “do it when needed.” Lazy work has not necessarily disappeared:
if the next operation needs everything, the cost returns. That is why the full
debugging journey is measured, not just plugin selection.

Code: [debug-map reader](../source/Plugins/SymbolFile/DWARF/SymbolFileDWARFDebugMap.cpp),
[symbol-file plugin](../source/Plugins/SymbolFile/Symtab/SymbolFileSymtab.cpp).

## 7. Problem: mapped memory is not always a complete file

Some system-library data comes from mapped regions, including shared-cache
views. Picture several readable pages with gaps between them. A large numerical
address range does not mean every byte in that range is available.

A file offset means "this many bytes into the file." A runtime address means
"this location in the running process." They are not interchangeable. Loading
and address randomization, often called ASLR, can change where code appears in
memory. The reader needs the correct mapping between these views.

**This draft's change:** validate the actual readable extent before creating a
subset view or using a lightweight metadata reader. Reject crossing a region
boundary, arithmetic overflow or a range outside its backing bytes. Use the
normal fallback when the shortcut cannot obtain the required complete view.

This matters for correctness as much as speed. Missing bytes must not become a
false “symbol absent” answer, an out-of-bounds read or a null value passed to a
caller that expects usable data. A **borrowed view** also has to remain within
the lifetime of the object that owns its bytes.

Code: [virtual-range validation](../source/Utility/VirtualDataExtractor.cpp),
[mapped-segment extents](../source/Host/macosx/objcxx/HostInfoMacOSX.mm),
[extractor tests](../unittests/Utility/VirtualDataExtractorTest.cpp).

## 8. Problem: lots of tiny conversations with the app

`debugserver` and LLDB communicate using a debugger protocol. Even on one
computer, repeatedly requesting tiny pieces of another process's memory has
overhead. “Remote debugging” refers to this control boundary; the app need not
be on a distant machine.

**This draft's change:** read bounded chunks of Mach-O load commands and reuse
bytes within that image scan. A **load command** is a record describing things
such as segments, identity and platform requirements. If a bulk read crosses
unreadable memory, try the exact required prefix where appropriate, preserving
the required versus optional information rules.

The draft also narrows a specific `qSymbol` request for dispatch-queue metadata
to its correct C-linkage name. `qSymbol` is a protocol exchange asking LLDB to
help resolve a symbol; unrelated requests keep their existing matching rules.

A faster image scan is useful, but its standalone improvement cannot simply be
added to improvements measured in another attach or expression experiment.

Code: [debugserver image scanning](../tools/debugserver/source/MacOSX/MachProcess.mm),
[protocol symbol lookup](../source/Plugins/Process/gdb-remote/GDBRemoteCommunicationClient.cpp),
[qSymbol tests](../test/API/functionalities/gdb_remote_client/TestqSymbol.py).

## 9. Problem: one librarian holds the only key

A global lock can protect a shared module list. If the debugger holds it while
constructing one expensive module, unrelated module construction may wait too.

**This draft's change:** keep shared-list lookup and publication protected, but
allow eligible module construction outside that lock. Before publishing,
reacquire the lock and check again: another caller, including a recursive
callback, may already have published the correct module.

That recheck is essential. Without it, two workers could publish competing
answers. The candidate must still satisfy the requested architecture, binary
identity and object-kind checks; stale cached modules must still be handled.

More threads are not automatically faster. Provider, module and section locks
still have contracts. This draft does not make every symbol search parallel.

Code: [shared module lookup and publication](../source/Core/ModuleList.cpp),
[concurrency tests](../unittests/Core/ModuleTest.cpp).

## 10. Problem: helpful debugger features do work nobody requested

The debugger probes for runtime helpers, including Bounds Safety support. These
checks can accidentally initialize symbol tables or human-readable name indexes
across many images.

**This draft's change:** use exact ABI names and conservative presence checks
for the relevant runtime probes and internal breakpoints. It does not turn off
the app's safety checks or suppress their stops.

Similarly, if the effective policy disables loading debug scripts for a module,
honour that policy before initializing symbol machinery just to discover those
scripts. This avoids unnecessary work without weakening the trust policy.

Code: [Bounds Safety runtime](../source/Plugins/InstrumentationRuntime/BoundsSafety/InstrumentationRuntimeBoundsSafety.cpp),
[Swift runtime detection](../source/Plugins/LanguageRuntime/Swift/SwiftLanguageRuntime.cpp),
[script policy](../source/Core/ModuleList.cpp),
[runtime tests](../unittests/Target/InstrumentationRuntimeTest.cpp).

## 11. Why does the first Swift expression need a compiler?

Printing a Swift value can require more than reading bytes at an address. LLDB
may need the value's type, imported declarations and generated code. Swift's
reflection machinery also reads runtime metadata; an optional symbol-based
shortcut should not force a cold full symbol table when metadata reading can
answer the question instead.

The expression compiler needs the right **compiler context**: module identity,
target platform and architecture, search paths, dependencies and relevant
options. C and Objective-C imports may involve **Clang**, a compiler frontend.
Its reusable module files are often called **PCMs**, or precompiled modules.

**Changes already in this draft:** deduplicate applicable AST-file loading per
consumer; support discovering the matching main module from its own embedded
Swift AST when external discovery fails; preserve module/triple validation,
ownership and compatibility fallbacks. Do not borrow an unrelated image's
similarly named module to make discovery appear successful.

**Work still being investigated:** repeated path normalization during module
discovery and the cost of first-use Clang imports/builds. Deduplicating file
loading does not automatically remove repeated work that happens before the
file is loaded. These observations are not a claim that the future fixes are
already in this snapshot.

A **fresh debugger cache** has no previous debugger module results to reuse.
A **reused cache** may avoid some compiler work. But a build's PCM is not
automatically suitable for LLDB: compiler identity, options, dependencies and
paths must agree. Skipping validation to force reuse could give incorrect types
or expressions.

Code: [Swift AST discovery](../source/Plugins/TypeSystem/Swift/SwiftASTContext.cpp),
[reflection reader](../source/Plugins/LanguageRuntime/Swift/LLDBMemoryReader.cpp),
[module-discovery tests](../test/API/lang/swift/explicit_modules/no_debugging_options/TestSwiftNoDebuggingOptions.py),
[reflection tests](../test/API/lang/swift/reflection_symbol_loading/TestSwiftReflectionSymbolLoading.py).

## 12. What do the performance numbers mean?

The anonymized development workload is a large modular Swift application with
thousands of loaded images. The deliberately rounded observations are:

| Checkpoint | Target setup through the first successful Swift expression |
| --- | --- |
| Original LLDB build | Roughly 1–1.5 minutes |
| Checked snapshot in this draft | Roughly 10–15 seconds |
| Research goal | Under one second; not achieved |

The clock begins before target setup and includes attach, source/locals setup
and the first successful expression. App launch before the debugger handshake
and LLDB process startup are outside this interval. Later expressions and
detach are outside it too; detach is still checked for correctness.

Important distinctions:

- **Wall time** is how long the person waits. CPU work from several threads
  can overlap, so adding their durations does not give wall time.
- **Sampling** periodically records what threads are doing. It helps locate
  work, but a short window does not describe an entire phase. A waiting thread
  is not proof that its wait can safely be removed.
- **Fresh debugger cache** does not mean cold OS file caches. The operating
  system may already have file data in memory.
- **Separate single runs** are observations, not a repeatable causal speedup.
  A stronger comparison repeats identity-matched baseline and patched runs
  under controlled conditions.
- **A shifted cost** is not a removed cost. Faster attach followed by a slower
  expression may leave the total unchanged.

These changes improve debugger work. They do not establish that the app itself
starts faster, runs faster in production or uses less battery.

## 13. How do we know the shortcut did not break debugging?

There are several layers of evidence, and each answers a different question:

| Check | What it can establish | What it cannot establish by itself |
| --- | --- | --- |
| Compilation | The selected source and dependencies build. | Correct behaviour or faster debugging. |
| Native unit tests | Targeted contracts for readers, symbols, caches and concurrency. | Every real program or every platform. |
| API tests | A real debugger can handle small Swift/C/C++/Objective-C and attach scenarios. | Large-app performance. |
| Application run | Source, locals, values, image identity and detach work in that workload. | Repeatability across apps and machines. |
| Profile | Where work appeared in the recorded interval. | A safe fix or a guaranteed saving. |
| CI | The configured checks ran on a particular pushed commit. | A pass when those jobs were skipped. |

The frozen source snapshot passed more than 150 selected active checks.
Intentional format/platform skips are counted separately. Cases include
malformed metadata, undefined names, stubs/resolvers, DWARF/dSYM selection,
partial mapped views, late-loaded runtimes and concurrent module publication.

The first implementation commit preserves that source snapshot. The separate
formatting commit has not been rebuilt. Existing upstream-only CI jobs skip
this fork, and their skipped status is not validation. Independent review and
broader platform coverage remain necessary.

An **artifact** is a particular built output. A **hash** identifies particular
bytes. Checking source, compiler, runtime and image identities prevents comparing
one patch's source with another patch's binary or accidentally reusing a stale
installation.

Finally, compiling LLDB on a remote build worker is different from remote
debugging. **RBE**, or remote build execution, moves build tasks to workers. A
build system such as Bazel can send declared inputs there and retrieve the
outputs. It does not require the source to have been published in a Git branch.

These caches also do different jobs:

| Cache | What it may save |
| --- | --- |
| Build-output cache | Compiling the same build inputs again. |
| Debugger/compiler module cache | Rebuilding compatible imported modules for expressions. |
| LLDB's in-memory metadata caches | Repeating suitable symbol or metadata preparation. |
| OS file cache | Fetching file data from storage again. |

One cache being warm does not prove the others are warm or compatible. Neither
remote compilation nor a build-cache hit proves that the resulting debugger is
correct or fast. This PR contains generic source and tests, not the private
application harness or build-service configuration.

## 14. How to read this draft without reading everything

Start with [the query contract](../include/lldb/Symbol/ObjectFile.h), then
[the Mach-O implementation](../source/Plugins/ObjectFile/Mach-O/ObjectFileMachO.cpp)
and [its regression tests](../unittests/Symbol/SymtabTest.cpp). Follow a specific
consumer, such as the Swift memory reader or Bounds Safety probe, to see why it
asks for that kind of answer.

For each shortcut, ask:

1. What exact question is this caller asking?
2. What information proves that expensive work is unnecessary?
3. Which unsupported, ambiguous or changing cases take the normal path?
4. Which test would catch an incorrect shortcut?
5. Did the full measured journey improve, or did the work move elsewhere?

This is a progress draft against a pinned Swift LLVM baseline. It collects
related experiments so they can be inspected together; it still needs coherent
upstream decomposition, broader review and repeated paired measurements.

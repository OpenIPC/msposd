# Project Instructions

This file is the single source of truth for all AI coding agents (Claude Code,
OpenAI Codex, or any tool that reads AGENTS.md / CLAUDE.md).

## Project Overview

A program that draws flight controller's OSD(On-Screen Display) information via MSP DisplayPort protocil directly onto the video stream coming from an OpenIPC camera.

## Development Workflow: spec - draft - simplify - verify

Every non-trivial task MUST follow this four-phase pipeline.
Each phase is a gate: do not advance until the current phase passes.

### Phase 1: Spec (Plan)

Before writing any code, produce a plan:

1. Read relevant documentation in `documentation/`.
2. Read the source files you intend to modify.
3. Write a concise plan covering: what changes, which files, why.
4. Document key design decisions and their rationale in the plan. This
   prevents oscillating between approaches mid-implementation.
5. Get human approval on the plan before proceeding.

Do NOT skip planning. A good plan lets you one-shot the implementation.

### Phase 2: Draft (Implement)

Execute the plan:

- Follow the coding conventions below.
- Make minimal, focused changes. Do not refactor unrelated code.
- Do not add features beyond what the spec calls for.

### Phase 3: Simplify (Review)

After implementation, review your own work:

- Can any function be shorter or clearer?
- Are there unnecessary abstractions, error paths, or comments?
- Does the architecture stay clean? No dead code, no orphan headers.
- Remove anything that is not strictly needed.

### Phase 4: Verify (Build + Test)

Run verification before declaring done:



## Documentation in code Rules:

Every function/method MUST include a documentation comment.

Requirements:
- Use standard language conventions.  
- Include:
  - Short function description (MAX 50 words)
  - Description of every parameter
  - Return value description if applicable
- Keep descriptions concise and technical.
- Do NOT exceed 50 words for the function summary.



# Copilot Project Guide – Haptique Kincony AGFW

## 🎯 Project Focus
This repository contains open-source firmware and tools for the **Haptique RS90 / Kincony AG Hub**.

Copilot should prioritize:
- Writing **clean ESP32 C++ / Arduino** code in `Haptique_AGFW.ino`
- Writing **Python 3** scripts for testing REST APIs under `tools/`
- Following the existing naming conventions:
  - `/api/wifi/save`
  - `/api/status`
  - `/api/ir/send`, `/api/ir/last`
  - `/api/rf/send`

## 🧱 Coding Style
- Use **snake_case** for Python and **camelCase** for C++.
- Always include meaningful comments.
- Follow existing indentation (2 spaces for Arduino, 4 for Python).
- Keep outputs human-readable (status ✅, errors ❌).

## 💡 Documentation
When adding new endpoints or tools:
- Include a short section in `docs/API_Reference.md`
- Update `CHANGELOG.md` with a `feat:` or `fix:` tag

## 🚫 Avoid
- Generating redundant flashing scripts.
- Using Windows-only commands unless explicitly mentioned.
- Adding external dependencies (stick to `requests`, `esptool.py`).

---

### Example Commit Messages
Use the [Conventional Commits](https://www.conventionalcommits.org/) format:
- `feat(api): add OTA update endpoint`
- `fix(ir): correct timing issue in send routine`
- `docs(readme): update usage section`

---

> 🧠 **Hint for Copilot:**  
> When asked to “extend tools” or “add endpoint handlers,” refer to this guide.

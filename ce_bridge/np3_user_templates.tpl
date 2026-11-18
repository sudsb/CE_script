-- encoding: UTF-8
-- User-editable templates for CE's "自动汇编 / 默认模板" submenu.
--
-- This file is deployed to <Cheat Engine>\autorun\np3_user_templates.tpl once,
-- only when it does not exist yet; your edits are never overwritten.  Edit it
-- with any text editor (Notepad3 CE menu -> "编辑 CE 默认模板"), then right
-- click the disassembler again: the menu refreshes automatically.
--
-- It is a plain Lua data file loaded in a sandbox.  Only pure Lua expressions
-- (string/table/math) are allowed: no CE API, no io/os, no side effects.
-- Available placeholders:
--   ${METHOD_SCAN}       AOB: aobscanmodule(...)/aobscan(...) line; address: empty
--   ${METHOD_REGISTER}   AOB: registersymbol(INJECT); address: empty
--   ${METHOD_UNREGISTER} AOB: unregistersymbol(INJECT); address: empty
--   ${INJECT_ADDR}       AOB: INJECT; address: the module+offset / hex address
--   ${ADDRESS}           module+offset (e.g. "game.exe"+21DC70) or hex address
--   ${HEX_ADDRESS}       start address in hex
--   ${ORIGINAL_CODE}     every selected instruction, commented out (// addr: text)
--   ${ORIGINAL_CODE_BARE} selected instructions as plain text
--   ${ORIG_BYTES}        original bytes, e.g. 48 39 D8
--   ${ORIG_BYTES_LINE}   db <original bytes>  (restore line)
--   ${NOP_COUNT}         number of padding bytes after the 5-byte jmp
--   ${NOP_COUNT_LINE}    nop <count> line (empty when no padding is needed)
--   ${NOPS}              one nop line per selected instruction
--   ${INSTRUCTION_COUNT} number of selected instructions
--   ${JUMP_TARGET}       operand of the first conditional jump
--   ${JMP_LINE}          jmp <original jump target>
--   ${CMP_EQUAL}         cmp <first operand>,<first operand>
--   ${SELECTED_TEXT}     selected instruction text
--   ${NEWLINE} ${TAB}
--
-- The selected original instructions are always kept as comments: if a body
-- does not reference ${ORIGINAL_CODE}, the bridge inserts the comment block
-- automatically.  Unknown placeholders abort generation with an error.
--
-- settings.method is the default generation method: 'aob' (default) or
-- 'address'.  A single template can override it with method = 'address'.
-- The method can also be switched from the "默认模板" submenu.

local function body(code)
  return table.concat({
    '[ENABLE]',
    '${METHOD_SCAN}',
    'alloc(newmem,2048,${INJECT_ADDR})',
    'label(returnhere)',
    'label(originalcode)',
    'label(exit)',
    '${METHOD_REGISTER}',
    '',
    'newmem:',
    'originalcode:',
    '// 原始代码 / original code (commented out):',
    '${ORIGINAL_CODE}',
    code,
    '',
    'exit:',
    'jmp returnhere',
    '',
    '${INJECT_ADDR}:',
    'jmp newmem',
    '${NOP_COUNT_LINE}',
    'returnhere:',
    '',
    '[DISABLE]',
    '${INJECT_ADDR}:',
    '${ORIG_BYTES_LINE}',
    'dealloc(newmem)',
    '${METHOD_UNREGISTER}'
  }, '\n')
end

return {
  settings = { method = 'aob' },
  templates = {
    { name = 'nop',   body = body('${NOPS}') },
    { name = 'ret 1', body = body('mov rax,1\nret') },
    { name = 'jmp',   body = body('${JMP_LINE}') },
    { name = 'cmp',   body = body('${CMP_EQUAL}') }
  }
}

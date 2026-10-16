import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import ts from 'typescript';
const output = ts.transpileModule(readFileSync(new URL('../src/panelVisibility.ts', import.meta.url), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.ESNext }
}).outputText;
const { panelShortcut, fitSidebars } = await import(`data:text/javascript;base64,${Buffer.from(output).toString('base64')}`);
const event = fields => ({ key:'b', code:'KeyB', metaKey:true, ctrlKey:false, shiftKey:false, altKey:false, isComposing:false, keyCode:0, getModifierState:()=>false, ...fields });
for (const mac of [true,false]) {
  const platform = {metaKey:mac,ctrlKey:!mac};
  assert.equal(panelShortcut(event(platform),mac),'explorer');
  assert.equal(panelShortcut(event({...platform,altKey:true,key:'∫'}),mac),'assistant');
  assert.equal(panelShortcut(event({...platform,code:'KeyJ',key:'j'}),mac),'dataflow');
  assert.equal(panelShortcut(event({...platform,code:'KeyJ',key:'J',shiftKey:true}),mac),'keyboard');
  for (const extra of [{isComposing:true},{keyCode:229},{getModifierState:()=>true},{shiftKey:true},{metaKey:false,ctrlKey:false},{metaKey:true,ctrlKey:true}])
    assert.equal(panelShortcut(event({...platform,...extra}),mac),null);
  assert.equal(panelShortcut(event({...platform,code:'KeyJ',altKey:true}),mac),null);
}
for (const width of [600,850,1440,2560]) for (const left of [false,true]) for (const right of [false,true]) {
  const layout=fitSidebars(width,240,320,left,right);
  assert.ok(layout.left>=0 && layout.right>=0 && layout.left+layout.right<=layout.budget+.001);
  assert.equal(layout.left===0,!left); assert.equal(layout.right===0,!right);
  if (width>=1440) { assert.equal(layout.left,left?240:0); assert.equal(layout.right,right?320:0); }
}
console.log('Panel visibility: Mac/Ctrl shortcuts, Option-produced characters and IME exclusions; independent sidebar geometry.');

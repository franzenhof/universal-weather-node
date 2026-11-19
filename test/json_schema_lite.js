// Minimal JSON Schema (2020-12 subset) validator for the API contract test —
// no npm dependency. Supported: type (string or array), properties, required,
// additionalProperties (boolean or schema), items, enum, const, minimum,
// maximum, minItems, maxItems, maxLength, pattern, $ref ("#/..." and
// "<file>#/..." relative to the referring schema), anyOf, oneOf. Unknown
// keywords (description, format, x-unit, ...) are annotations and ignored.
const fs = require('node:fs');
const path = require('node:path');

class SchemaSet {
  constructor(dir) {
    this.dir = dir;
    this.files = {};
  }

  file(name) {
    if (!this.files[name]) this.files[name] = JSON.parse(fs.readFileSync(path.join(this.dir, name), 'utf8'));
    return this.files[name];
  }

  // -> {schema, file}: follows $ref chains; siblings of $ref are annotations.
  resolve(schema, file) {
    let guard = 0;
    while (schema && schema.$ref) {
      if (++guard > 20) throw new Error(`$ref loop in ${file}`);
      const [target, pointer = ''] = schema.$ref.split('#');
      if (target) file = target;
      let s = this.file(file);
      for (const part of pointer.split('/').filter(Boolean)) {
        s = s[part.replace(/~1/g, '/').replace(/~0/g, '~')];
        if (s === undefined) throw new Error(`unresolvable $ref ${schema.$ref} in ${file}`);
      }
      schema = s;
    }
    return {schema, file};
  }

  validate(value, file, schema = this.file(file), at = '$', errors = []) {
    ({schema, file} = this.resolve(schema, file));
    if (schema === true || schema === undefined) return errors;
    if (schema === false) { errors.push(`${at}: not allowed`); return errors; }
    const err = (msg) => errors.push(`${at}: ${msg}`);

    if (schema.type !== undefined) {
      const types = Array.isArray(schema.type) ? schema.type : [schema.type];
      if (!types.some((t) => typeOk(value, t))) {
        err(`type ${jsonType(value)}, expected ${types.join('|')}`);
        return errors;
      }
    }
    if (schema.enum && !schema.enum.some((e) => e === value)) err(`${JSON.stringify(value)} not in enum`);
    if ('const' in schema && schema.const !== value) err(`${JSON.stringify(value)} != const ${JSON.stringify(schema.const)}`);
    if (typeof value === 'number') {
      if (schema.minimum !== undefined && value < schema.minimum) err(`${value} < minimum ${schema.minimum}`);
      if (schema.maximum !== undefined && value > schema.maximum) err(`${value} > maximum ${schema.maximum}`);
    }
    if (typeof value === 'string') {
      if (schema.maxLength !== undefined && value.length > schema.maxLength) err(`longer than ${schema.maxLength}`);
      if (schema.pattern && !new RegExp(schema.pattern, 'u').test(value)) err(`${JSON.stringify(value)} does not match ${schema.pattern}`);
    }
    if (Array.isArray(value)) {
      if (schema.minItems !== undefined && value.length < schema.minItems) err(`fewer than ${schema.minItems} items`);
      if (schema.maxItems !== undefined && value.length > schema.maxItems) err(`more than ${schema.maxItems} items`);
      if (schema.items !== undefined) value.forEach((v, i) => this.validate(v, file, schema.items, `${at}[${i}]`, errors));
    }
    if (isObject(value)) {
      for (const r of schema.required || []) if (!(r in value)) err(`missing required "${r}"`);
      const props = schema.properties || {};
      for (const [k, v] of Object.entries(value)) {
        if (k in props) this.validate(v, file, props[k], `${at}.${k}`, errors);
        else if (schema.additionalProperties !== undefined) this.validate(v, file, schema.additionalProperties, `${at}.${k}`, errors);
      }
    }
    for (const kw of ['anyOf', 'oneOf']) {
      if (!schema[kw]) continue;
      const ok = schema[kw].filter((s) => this.validate(value, file, s, at, []).length === 0).length;
      if (kw === 'anyOf' ? ok === 0 : ok !== 1) err(`${kw}: ${ok} branches match`);
    }
    return errors;
  }

  // Keys present in `value` that the schema does not describe by name
  // (additionalProperties allows them, so validate() alone accepts them).
  undocumented(value, file, schema = this.file(file), at = '$', out = []) {
    ({schema, file} = this.resolve(schema, file));
    if (!schema || typeof schema !== 'object') return out;
    if (Array.isArray(value) && schema.items !== undefined) {
      value.forEach((v, i) => this.undocumented(v, file, schema.items, `${at}[${i}]`, out));
    } else if (isObject(value)) {
      const props = schema.properties || {};
      for (const [k, v] of Object.entries(value)) {
        if (k in props) this.undocumented(v, file, props[k], `${at}.${k}`, out);
        else out.push(`${at}.${k}`);
      }
    }
    return out;
  }

  // Every named property in the schema file carries a description.
  undescribed(file) {
    const out = [];
    const walk = (s, at) => {
      if (!s || typeof s !== 'object') return;
      for (const [k, p] of Object.entries(s.properties || {})) {
        if (!p.description) out.push(`${at}.${k}`);
        walk(p, `${at}.${k}`);
      }
      if (s.items) walk(s.items, `${at}[]`);
      for (const [k, d] of Object.entries(s.$defs || {})) walk(d, `$defs.${k}`);
    };
    walk(this.file(file), '$');
    return out;
  }

  // Number of named properties (documented fields) in a schema file.
  countFields(file) {
    let n = 0;
    const walk = (s) => {
      if (!s || typeof s !== 'object') return;
      for (const p of Object.values(s.properties || {})) { n++; walk(p); }
      if (s.items) walk(s.items);
      for (const d of Object.values(s.$defs || {})) walk(d);
    };
    walk(this.file(file));
    return n;
  }
}

function isObject(v) { return v !== null && typeof v === 'object' && !Array.isArray(v); }
function jsonType(v) {
  if (v === null) return 'null';
  if (Array.isArray(v)) return 'array';
  if (typeof v === 'number') return Number.isInteger(v) ? 'integer' : 'number';
  return typeof v;
}
function typeOk(v, t) {
  if (t === 'number') return typeof v === 'number' && Number.isFinite(v);
  if (t === 'integer') return Number.isInteger(v);
  if (t === 'object') return isObject(v);
  return jsonType(v) === t;
}

module.exports = {SchemaSet};

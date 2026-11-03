import re
import os
class Sym(str): pass
TOK=re.compile(r'\s*(\(|\)|"(?:[^"\\]|\\.)*"|[^\s()"]+)')
def parse(s):
    pos=0; stack=[[]]
    for m in TOK.finditer(s):
        t=m.group(1)
        if t=='(':
            stack.append([])
        elif t==')':
            l=stack.pop(); stack[-1].append(l)
        elif t.startswith('"'):
            stack[-1].append(t[1:-1].replace('\\"','"').replace('\\\\','\\'))
        else:
            stack[-1].append(Sym(t))
    return stack[0]
def q(s): return '"'+s.replace('\\','\\\\').replace('"','\\"').replace('\n','\\n')+'"'
def dump(x, ind=0):
    if isinstance(x,list):
        if not x: return '()'
        simple=all(not isinstance(e,list) for e in x)
        if simple: return '('+' '.join(dump(e) for e in x)+')'
        out='('+dump(x[0])
        for e in x[1:]:
            out+='\n'+'  '*(ind+1)+dump(e,ind+1) if isinstance(e,list) else ' '+dump(e)
        return out+')'
    if isinstance(x,Sym): return str(x)
    if isinstance(x,bool): return 'yes' if x else 'no'
    if isinstance(x,(int,float)): 
        s=('%.4f'%x).rstrip('0').rstrip('.'); return s if s not in('-0','') else '0'
    return q(x)
def find(l,key): return [e for e in l if isinstance(e,list) and e and e[0]==key]
def get(l,key):
    r=find(l,key); return r[0] if r else None
_libcache={}
LIBDIR='/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols'
LOCAL_LIBDIR = os.path.dirname(os.path.abspath(__file__))
def libsym(lib,name):
    if lib not in _libcache:
        libdir = LOCAL_LIBDIR if lib == "WeatherNode" else LIBDIR
        _libcache[lib]=parse(open(f'{libdir}/{lib}.kicad_sym').read())[0]
    L=_libcache[lib]
    for s in find(L,'symbol'):
        if s[1]==name: return s
    raise KeyError(name)
def flat(lib,name):
    """return flattened copy of symbol (resolving extends), named lib:name"""
    import copy
    s=copy.deepcopy(libsym(lib,name))
    ext=get(s,'extends')
    if ext:
        base=flat(lib,ext[1])
        props={p[1]:p for p in find(s,'property')}
        new=[Sym('symbol'), f'{lib}:{name}']
        for e in base[2:]:
            if isinstance(e,list) and e[0]=='property' and e[1] in props: new.append(props.pop(e[1]))
            elif isinstance(e,list) and e[0]=='symbol':
                e=copy.deepcopy(e); e[1]=e[1].replace(ext[1].split(':')[-1],name,1); new.append(e)
            else: new.append(e)
        for p in props.values(): new.append(p)
        return new
    s[1]=f'{lib}:{name}'
    return s
def pins(sym, unit=1):
    out=[]
    for sub in find(sym,'symbol'):
        m=re.search(r'_(\d+)_(\d+)$',sub[1]); u=int(m.group(1))
        if u not in (0,unit): continue
        for p in find(sub,'pin'):
            at=get(p,'at'); out.append(dict(type=str(p[1]),name=get(p,'name')[1],num=get(p,'number')[1],x=float(at[1]),y=float(at[2]),a=float(at[3]) if len(at)>3 else 0.0,hidden='hide' in [str(e) for e in p] or any(isinstance(e,list) and e[0]=='hide' for e in p)))
    return out

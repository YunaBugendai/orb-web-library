#!/usr/bin/env python3
"""Differential fuzzer: random Orb! programs vs. a Python reference implementation.
usage: python3 tests/fuzz.py [count] [seed]"""
import random, subprocess, sys, os, tempfile

M = (1 << 64) - 1
def wrap(x):
    x &= M
    return x - (1 << 64) if x >> 63 else x
def trunc(x, bits, signed):
    x &= (1 << bits) - 1
    if signed and x >> (bits - 1): x -= 1 << bits
    return x

class Gen:
    def __init__(self, rnd):
        self.r = rnd
        self.nvars = rnd.randint(4, 14)
        self.funcs = []   # (name, nparams, body_expr_builder)
    def var(self): return "v%d" % self.r.randrange(self.nvars)
    def expr(self, depth, scope):
        r = self.r
        if depth <= 0 or r.random() < 0.2:
            if r.random() < 0.6: return ("var", r.choice(scope))
            return ("const", r.randint(-1000, 1000))
        k = r.random()
        if k < 0.55:
            op = r.choice(["+", "-", "*", "&", "|", "^"])
            return ("bin", op, self.expr(depth-1, scope), self.expr(depth-1, scope))
        if k < 0.65:
            op = r.choice(["/", "%"])
            return ("divc", op, self.expr(depth-1, scope), r.choice([1, 2, 3, 7, 8, 10, 16, 100, 1024, -3, -8]))
        if k < 0.75:
            op = r.choice(["<<", ">>"])
            return ("shift", op, self.expr(depth-1, scope), r.randint(0, 40))
        if k < 0.85:
            ty = r.choice([("u8", 8, False), ("i8", 8, True), ("u16", 16, False), ("i16", 16, True), ("i32", 32, True), ("u32", 32, False)])
            return ("cast", ty, self.expr(depth-1, scope))
        if k < 0.95:
            op = r.choice(["<", "<=", ">", ">=", "==", "!="])
            return ("cmp", op, self.expr(depth-1, scope), self.expr(depth-1, scope),
                    self.expr(depth-1, scope), self.expr(depth-1, scope))
        return ("neg", self.expr(depth-1, scope))

def show(e):
    t = e[0]
    if t == "var": return e[1]
    if t == "const": return "(%d)" % e[1]
    if t == "bin": return "(%s %s %s)" % (show(e[2]), e[1], show(e[3]))
    if t == "divc": return "(%s %s (%d))" % (show(e[2]), e[1], e[3])
    if t == "shift": return "(%s %s %d)" % (show(e[2]), e[1], e[3])
    if t == "cast": return "((%s as %s) as i64)" % (show(e[2]), e[1][0])
    if t == "cmp": return "(if_cmp(%s %s %s, %s, %s))" % (show(e[2]), e[1], show(e[3]), show(e[4]), show(e[5]))
    if t == "neg": return "(-%s)" % show(e[1])

def show_orb(e, helpers):
    t = e[0]
    if t == "var": return e[1]
    if t == "const": return "(%d)" % e[1] if e[1] >= 0 else "(0 - %d)" % -e[1]
    if t == "bin": return "(%s %s %s)" % (show_orb(e[2], helpers), e[1], show_orb(e[3], helpers))
    if t == "divc": return "(%s %s (%d))" % (show_orb(e[2], helpers), e[1], e[3])
    if t == "shift": return "(%s %s %d)" % (show_orb(e[2], helpers), e[1], e[3])
    if t == "cast": return "((%s as %s) as i64)" % (show_orb(e[2], helpers), e[1][0])
    if t == "cmp":
        helpers.add(1)
        return "sel(%s %s %s, %s, %s)" % (show_orb(e[2], helpers), e[1], show_orb(e[3], helpers), show_orb(e[4], helpers), show_orb(e[5], helpers))
    if t == "neg": return "(0 - %s)" % show_orb(e[1], helpers)

def ev(e, env):
    t = e[0]
    if t == "var": return env[e[1]]
    if t == "const": return e[1]
    if t == "bin":
        a, b = ev(e[2], env), ev(e[3], env)
        return wrap({"+": a + b, "-": a - b, "*": a * b, "&": a & b, "|": a | b, "^": a ^ b}[e[1]])
    if t == "divc":
        a, d = ev(e[2], env), e[3]
        q = abs(a) // abs(d)
        if (a < 0) != (d < 0): q = -q
        if e[1] == "/": return wrap(q)
        return wrap(a - q * d)
    if t == "shift":
        a = ev(e[2], env)
        return wrap(a << e[3]) if e[1] == "<<" else a >> e[3]
    if t == "cast":
        return trunc(ev(e[2], env), e[1][1], e[1][2])
    if t == "cmp":
        a, b = ev(e[2], env), ev(e[3], env)
        c = {"<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b, "==": a == b, "!=": a != b}[e[1]]
        x, y = ev(e[4], env), ev(e[5], env)
        return x if c else y
    if t == "neg": return wrap(-ev(e[1], env))

def gen_program(seed):
    rnd = random.Random(seed)
    g = Gen(rnd)
    nv = g.nvars
    vars_ = ["v%d" % i for i in range(nv)]
    init = [rnd.randint(-50, 50) for _ in range(nv)]
    # helper functions with 1..8 params
    nf = rnd.randint(1, 3)
    funcs = []
    for fi in range(nf):
        np_ = rnd.randint(1, 8)
        params = ["p%d" % i for i in range(np_)]
        body = g.expr(3, params)
        funcs.append((fi, params, body))
    lines = []
    helpers = set()
    sim = []   # list of actions for python
    env = dict(zip(vars_, init))
    out_orb = []
    # statements
    stmts = []
    for _ in range(rnd.randint(8, 24)):
        k = rnd.random()
        if k < 0.55:
            stmts.append(("assign", rnd.choice(vars_), g.expr(4, vars_)))
        elif k < 0.7:
            fi, params, _b = rnd.choice(funcs)
            args = [g.expr(2, vars_) for _ in params]
            stmts.append(("call", rnd.choice(vars_), fi, args))
        elif k < 0.85:
            cond = ("cmp", rnd.choice(["<", ">", "==", "!="]), g.expr(2, vars_), g.expr(2, vars_), ("const", 1), ("const", 0))
            stmts.append(("if", cond, rnd.choice(vars_), g.expr(3, vars_), rnd.choice(vars_), g.expr(3, vars_)))
        else:
            n = rnd.randint(1, 12)
            stmts.append(("loop", n, rnd.choice(vars_), g.expr(3, vars_ + ["i"]), rnd.choice(vars_), g.expr(3, vars_ + ["i"])))
    # emit orb
    o = []
    for (fi, params, body) in funcs:
        o.append("fn f%d(%s) -> i64 { return %s; }" % (fi, ", ".join("%s: i64" % p for p in params), show_orb(body, helpers)))
    o.append("fn sel(c: bool, a: i64, b: i64) -> i64 { if c { return a; } return b; }")
    o.append("fn main() {")
    for v, x in zip(vars_, init): o.append("    %s: i64 = %s;" % (v, "(0 - %d)" % -x if x < 0 else str(x)))
    for s in stmts:
        if s[0] == "assign": o.append("    %s = %s;" % (s[1], show_orb(s[2], helpers)))
        elif s[0] == "call":
            o.append("    %s = f%d(%s);" % (s[1], s[2], ", ".join(show_orb(a, helpers) for a in s[3])))
        elif s[0] == "if":
            o.append("    if %s != 0 { %s = %s; } else { %s = %s; }" % (show_orb(s[1], helpers), s[2], show_orb(s[3], helpers), s[4], show_orb(s[5], helpers)))
        else:
            o.append("    for i in 0..%d { %s = %s; if i %% 2 == 1 { %s = %s; } }" % (s[1], s[2], show_orb(s[3], helpers), s[4], show_orb(s[5], helpers)))
    for v in vars_: o.append("    println(%s);" % v)
    o.append("}")
    # python reference
    for s in stmts:
        if s[0] == "assign": env[s[1]] = ev(s[2], env)
        elif s[0] == "call":
            fi = s[2]; params = funcs[fi][1]
            fe = {p: ev(a, env) for p, a in zip(params, s[3])}
            env[s[1]] = ev(funcs[fi][2], fe)
        elif s[0] == "if":
            if ev(s[1], env) != 0: env[s[2]] = ev(s[3], env)
            else: env[s[4]] = ev(s[5], env)
        else:
            for i in range(s[1]):
                env2 = dict(env); env2["i"] = i
                env[s[2]] = ev(s[3], env2)
                if i % 2 == 1:
                    env2 = dict(env); env2["i"] = i
                    env[s[4]] = ev(s[5], env2)
    expected = "\n".join(str(env[v]) for v in vars_) + "\n"
    return "\n".join(o) + "\n", expected

def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    seed0 = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    orb = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "orb")
    tmp = tempfile.mkdtemp()
    bad = 0
    for k in range(count):
        seed = seed0 + k
        src, exp = gen_program(seed)
        p = os.path.join(tmp, "f.orb")
        open(p, "w").write(src)
        for flags in ([], ["--no-opt"]):
            r = subprocess.run([orb, "build", p, "-o", os.path.join(tmp, "f.bin")] + flags, capture_output=True, text=True)
            if r.returncode != 0:
                print("seed", seed, flags, "COMPILE ERROR:", r.stderr[:300]); bad += 1; open("/tmp/fuzz_fail_%d.orb" % seed, "w").write(src); break
            r = subprocess.run([os.path.join(tmp, "f.bin")], capture_output=True, text=True)
            if r.stdout != exp:
                print("seed", seed, flags, "MISMATCH"); bad += 1
                open("/tmp/fuzz_fail_%d.orb" % seed, "w").write(src)
                break
    print("fuzz: %d programs, %d failures" % (count, bad))
    sys.exit(1 if bad else 0)
main()

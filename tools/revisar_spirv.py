#!/usr/bin/env python3
"""Revisa los shaders traducidos contra lo que admite una GPU, sin la GPU: para saber qué puede fallar en una Mali (los
MediaTek, los Exynos) o en el driver propio de Qualcomm antes de tener el teléfono en la mano.

Lee la caché de SPIR-V del juego (<datos>/spirv.bin, la del teléfono que da problemas: sus traducciones llevan las
opciones de su GPU), la carpeta <datos>/pipelines_fallidas o ficheros .spv sueltos, y por cada shader dice lo que usa
de lo que suele romper:
  - operaciones de subgrupo (votos, ballot, shuffle), con el tamaño de subgrupo de la GPU por debajo del warp de 32
  - escrituras o atómicos en buffers desde vertex / geometry (la Mali-G615 no tiene vertexPipelineStoresAndAtomics)
  - memoria compartida por encima de maxComputeSharedMemorySize
  - la decoración NonUniform en los índices de arrays de texturas, arrays de descriptores sin tamaño
  - enteros de 64 bits, half floats, geometry y teselación, demote / terminate

Uso: python3 tools/revisar_spirv.py [--gpu mali-g615|adreno-730|adreno-650] <spirv.bin | carpeta | .spv>... [--todos]
     (sin --todos, solo los shaders con algún aviso)
"""
import os
import struct
import sys
from collections import Counter

# Lo que importa de cada GPU (de los logs del port: "subgrupos", "features", y los límites de su Vulkan)
GPUS = {
    "mali-g615": dict(subgroup=16, vertex_stores=False, shared=32768, int64=True, float16=True),
    "adreno-730": dict(subgroup=64, vertex_stores=True, shared=32768, int64=True, float16=True),
    "adreno-650": dict(subgroup=64, vertex_stores=True, shared=32768, int64=True, float16=True),
}

MAGIC = 0x07230203
MODELS = {0: "vertex", 1: "tess control", 2: "tess eval", 3: "geometry", 4: "pixel", 5: "compute"}
CAPS = {9: "Float16", 10: "Float64", 11: "Int64", 12: "Int64Atomics", 22: "Int16", 39: "Int8", 2: "Geometry",
        3: "Tessellation", 61: "GroupNonUniform", 62: "GroupNonUniformVote", 63: "GroupNonUniformArithmetic",
        64: "GroupNonUniformBallot", 65: "GroupNonUniformShuffle", 66: "GroupNonUniformShuffleRelative",
        68: "GroupNonUniformQuad", 5301: "ShaderNonUniform", 5302: "RuntimeDescriptorArray",
        5379: "DemoteToHelperInvocation"}
# OpGroupNonUniform*: 333..366
SUBGROUP_OPS = {334: "All", 335: "Any", 336: "AllEqual", 337: "Broadcast", 338: "BroadcastFirst", 339: "Ballot",
                345: "Shuffle", 346: "ShuffleXor", 347: "ShuffleUp", 348: "ShuffleDown", 365: "QuadBroadcast",
                366: "QuadSwap"}
VOTES = {"All", "Any", "AllEqual", "Ballot"}
SC_UNIFORM, SC_WORKGROUP, SC_STORAGE_BUFFER = 2, 4, 12
ATOMICS = set(range(227, 243))  # OpAtomicLoad … OpAtomicXor


def words_of(data):
    return list(struct.unpack("<%dI" % (len(data) // 4), data[: len(data) // 4 * 4]))


def modules_in_cache(data):
    """Los módulos de una caché (spirv_cache.cpp: etapa, número de palabras y las palabras de cada traducción)."""
    w = words_of(data)
    out = []
    for i in range(2, len(w)):
        if w[i] != MAGIC:
            continue
        n, stage = w[i - 1], w[i - 2]
        if 5 <= n <= len(w) - i and stage <= 5:
            out.append(w[i : i + n])
    return out


def type_size(types, tid):
    t = types.get(tid)
    if not t:
        return 0
    kind = t[0]
    if kind in ("int", "float"):
        return t[1] // 8
    if kind == "bool":
        return 4
    if kind in ("vector", "matrix"):
        return type_size(types, t[1]) * t[2]
    if kind == "array":
        return type_size(types, t[1]) * t[2]
    if kind == "struct":
        return sum(type_size(types, m) for m in t[1])
    return 0


def analyze(w):
    """Lo que interesa de un módulo SPIR-V."""
    caps, models, ops = set(), set(), Counter()
    nonuniform = 0
    types, consts, ptr_class, var_class = {}, {}, {}, {}
    shared = 0
    buffer_writes = 0
    i = 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xFFFF
        if n == 0:
            break
        a = w[i + 1 : i + n]
        if op == 17:
            caps.add(a[0])
        elif op == 15:
            models.add(a[0])
        elif op == 71 and len(a) >= 2 and a[1] == 5300:  # OpDecorate NonUniform
            nonuniform += 1
        elif op in (21,):  # OpTypeInt
            types[a[0]] = ("int", a[1])
        elif op == 22:  # OpTypeFloat
            types[a[0]] = ("float", a[1])
        elif op == 20:
            types[a[0]] = ("bool",)
        elif op == 23:  # OpTypeVector
            types[a[0]] = ("vector", a[1], a[2])
        elif op == 24:  # OpTypeMatrix
            types[a[0]] = ("matrix", a[1], a[2])
        elif op == 28:  # OpTypeArray (longitud: una constante)
            types[a[0]] = ("array", a[1], consts.get(a[2], 0))
        elif op == 29:  # OpTypeRuntimeArray
            types[a[0]] = ("runtime", a[1])
        elif op == 30:  # OpTypeStruct
            types[a[0]] = ("struct", list(a[1:]))
        elif op == 32:  # OpTypePointer
            ptr_class[a[0]] = a[1]
            types[a[0]] = ("pointer", a[1], a[2])
        elif op == 43 and len(a) >= 3:  # OpConstant
            consts[a[1]] = a[2]
        elif op == 59:  # OpVariable: tipo, id, clase
            var_class[a[1]] = a[2]
            if a[2] == SC_WORKGROUP:
                pt = types.get(a[0])
                if pt and pt[0] == "pointer":
                    shared += type_size(types, pt[2])
        elif op in (65, 66, 67):  # OpAccessChain…: la clase de su base
            if a[2] in var_class:
                var_class[a[1]] = var_class[a[2]]
        elif op == 62:  # OpStore puntero, valor
            if var_class.get(a[0]) in (SC_STORAGE_BUFFER, SC_UNIFORM):
                buffer_writes += 1
        elif op in ATOMICS:
            ptr = a[2] if op != 228 else a[0]  # (OpAtomicStore: puntero primero)
            if var_class.get(ptr) in (SC_STORAGE_BUFFER, SC_UNIFORM):
                buffer_writes += 1
        if op in SUBGROUP_OPS:
            ops[SUBGROUP_OPS[op]] += 1
        elif 333 <= op <= 366:
            ops["otra(%d)" % op] += 1
        elif op == 4416:
            ops["TerminateInvocation"] += 1
        elif op == 5380:
            ops["Demote"] += 1
        i += n
    return dict(caps=caps, models=models, ops=ops, nonuniform=nonuniform, shared=shared, buffer_writes=buffer_writes,
                runtime_arrays=sum(1 for t in types.values() if t[0] == "runtime"))


def warnings(info, gpu):
    out = []
    stages = {MODELS.get(m, str(m)) for m in info["models"]}
    votes = [k for k in info["ops"] if k in VOTES]
    if info["ops"] and gpu["subgroup"] < 32:
        out.append("subgrupo (%s) con subgrupos de %d < warp 32%s" % (
            ", ".join("%s x%d" % kv for kv in sorted(info["ops"].items())), gpu["subgroup"],
            ": votos de medio warp" if votes else ""))
    if info["buffer_writes"] and stages & {"vertex", "geometry", "tess control", "tess eval"} and not gpu["vertex_stores"]:
        out.append("%d escrituras/atómicos en buffers desde %s (la GPU no tiene vertexPipelineStoresAndAtomics)" % (
            info["buffer_writes"], "/".join(sorted(stages))))
    if info["shared"] > gpu["shared"]:
        out.append("memoria compartida %d bytes > %d de la GPU" % (info["shared"], gpu["shared"]))
    if 11 in info["caps"] and not gpu["int64"]:
        out.append("enteros de 64 bits sin shaderInt64")
    if 9 in info["caps"] and not gpu["float16"]:
        out.append("half floats sin shaderFloat16")
    if info["nonuniform"]:
        out.append("NonUniform x%d (índices de arrays de texturas)" % info["nonuniform"])
    if info["runtime_arrays"]:
        out.append("%d arrays sin tamaño (runtimeDescriptorArray o buffers)" % info["runtime_arrays"])
    if "geometry" in stages:
        out.append("geometry shader (lento en Mali)")
    for k in ("TerminateInvocation", "Demote"):
        if info["ops"].get(k):
            out.append(k)
    return out


def sources(paths):
    for p in paths:
        if os.path.isdir(p):
            for name in sorted(os.listdir(p)):
                if name.endswith(".spv"):
                    yield os.path.join(p, name), [words_of(open(os.path.join(p, name), "rb").read())]
        elif p.endswith(".spv"):
            yield p, [words_of(open(p, "rb").read())]
        else:
            yield p, modules_in_cache(open(p, "rb").read())


def main():
    args = sys.argv[1:]
    gpu_name = "mali-g615"
    if "--gpu" in args:
        k = args.index("--gpu")
        gpu_name = args[k + 1]
        del args[k : k + 2]
    every = "--todos" in args
    args = [a for a in args if a != "--todos"]
    if not args or gpu_name not in GPUS:
        print(__doc__)
        print("GPUs:", ", ".join(GPUS))
        sys.exit(1)
    gpu = GPUS[gpu_name]
    totals, shaders, flagged = Counter(), 0, 0
    for path, modules in sources(args):
        for idx, w in enumerate(modules):
            if len(w) < 5 or w[0] != MAGIC:
                continue
            shaders += 1
            info = analyze(w)
            found = warnings(info, gpu)
            for f in found:
                totals[f.split(" ")[0]] += 1
            if found:
                flagged += 1
            if found or every:
                stages = "/".join(sorted(MODELS.get(m, "?") for m in info["models"])) or "?"
                print("%s #%d (%s, %d palabras):" % (os.path.basename(path), idx, stages, len(w)))
                for f in found or ["sin avisos"]:
                    print("    " + f)
    print("\n%d shaders, %d con avisos para %s" % (shaders, flagged, gpu_name))
    for k, v in totals.most_common():
        print("  %-12s %d" % (k, v))


if __name__ == "__main__":
    main()

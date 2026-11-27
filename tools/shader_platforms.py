# Which graphics APIs the build's shaders exist for: the "platforms" list of Shader objects.
import UnityPy, collections, sys, time
NAMES = {4: "d3d11", 15: "glcore", 9: "gles3", 18: "vulkan", 21: "vulkan?", 16: "d3d12?", 14: "metal"}
plat = collections.Counter(); n = 0; t0 = time.time()
for path in sys.argv[1:]:
    e = UnityPy.load(path)
    for obj in e.objects:
        if obj.type.name != "Shader":
            continue
        t = obj.read_typetree()
        ps = t.get("platforms") or []
        plat[tuple(ps)] += 1
        n += 1
        if n <= 3:
            print("sample", t.get("m_ParsedForm", {}).get("m_Name"), ps, flush=True)
        if n >= 25:
            break
    print(path, n, dict(plat), round(time.time() - t0), "s", flush=True)
    if n >= 25:
        break
print("RESULT", n, dict(plat))

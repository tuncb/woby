"""Draw report diagrams from measured stages and the loader's buffer ownership.

This is document content. It does not instrument or change the application.
"""
import html
from svg_embed import namespace_svg
import json
import statistics
import textwrap

import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch


PALETTE = {"input": "#fff3e6", "output": "#edf4f4", "process": "#216e76"}
INK, MUTED = "#203139", "#586e77"
WIDTH = 1120


def seconds(records, phase):
    return statistics.median(sum(event["wall_ms"] for event in record["events"]
                                 if event["name"] == phase) for record in records) / 1000


def folded(text, width):
    return "\n".join(line for paragraph in text.split("\n")
                     for line in textwrap.wrap(paragraph, width, break_long_words=False,
                                               break_on_hyphens=False) or [""])


def box(ax, bounds, title, body, kind="output", url=None):
    x, y, width, height = bounds
    patch = FancyBboxPatch((x, y), width, height, boxstyle="round,pad=0,rounding_size=9",
                          facecolor=PALETTE[kind], edgecolor="none")
    ax.add_patch(patch)
    if url:
        patch.set_url("report.html#" + url)
    color = "white" if kind == "process" else INK
    heading = folded(title, max(20, int(width / 7.8)))
    heading_text = ax.text(x + 16, y + 16, heading, ha="left", va="top", color=color,
                           fontsize=14, weight="bold", linespacing=1.35)
    offset = 20 * (heading.count("\n") + 1)
    body_text = ax.text(x + 16, y + 23 + offset, folded(body, max(24, int(width / 6.7))),
                        ha="left", va="top", color=color, fontsize=12, linespacing=1.4)
    if url:
        heading_text.set_url("report.html#" + url)
        body_text.set_url("report.html#" + url)
    return patch


def arrow(ax, start, finish, label="", dashed=False, via=None):
    points = [start, *(via or []), finish]
    for i in range(len(points) - 1):
        ax.add_patch(FancyArrowPatch(points[i], points[i + 1],
            arrowstyle="-|>" if i == len(points) - 2 else "-", mutation_scale=13,
            linewidth=1.25, linestyle="--" if dashed else "-", color=MUTED,
            shrinkA=2, shrinkB=3))
    if label:
        first, last = points[0], points[-1]
        ax.text((first[0] + last[0]) / 2, (first[1] + last[1]) / 2 - 9,
                label, ha="center", va="bottom", color=MUTED, fontsize=11,
                bbox={"facecolor": "white", "edgecolor": "none", "pad": 2})


def canvas(height):
    fig = plt.figure(figsize=(WIDTH / 72, height / 72))
    ax = fig.add_axes((0, 0, 1, 1))
    ax.set_xlim(0, WIDTH)
    ax.set_ylim(height, 0)
    ax.axis("off")
    return fig, ax


def save(fig, destination, name, description):
    plt.rcParams["svg.fonttype"] = "none"
    plt.rcParams["svg.hashsalt"] = name
    fig.savefig(destination / (name + ".svg"))
    fig.savefig(destination / (name + ".png"), dpi=110)
    plt.close(fig)
    raw = (destination / (name + ".svg")).read_text(encoding="utf-8")
    svg = "<svg" + raw.split("<svg", 1)[1]
    svg = svg.replace('target="_blank"', 'target="_self"')
    svg = svg.replace("<svg ", '<svg role="img" aria-label="' + html.escape(description, quote=True) + '" ', 1)
    svg = svg.replace(">", "><title>" + html.escape(description) + "</title>", 1)
    (destination / (name + ".svg")).write_text(svg, encoding="utf-8")
    return namespace_svg(svg.replace('xlink:href="report.html#', 'xlink:href="#'), name + '-')


def rows_diagram(destination, name, title, steps):
    row_heights = []
    for step in steps:
        lengths = [folded(step[key], 43).count("\n") + 1 for key in ("input", "output", "action")]
        row_heights.append(max(136, 78 + max(lengths) * 17))
    height = 72 + sum(row_heights) + (len(steps) - 1) * 45 + 34
    fig, ax = canvas(height)
    ax.text(20, 19, title, fontsize=17, weight="bold", color=INK, va="top")
    for x, label in ((20, "INPUT: existing data"), (417, "STEP: work done"), (767, "OUTPUT: data or state")):
        ax.text(x, 49, label, fontsize=11, color=MUTED)
    y = 72
    for i, (step, row_height) in enumerate(zip(steps, row_heights)):
        anchor = "flow-step-" + step["id"]
        box(ax, (20, y, 332, row_height), step["input_title"], step["input"], "input")
        box(ax, (417, y, 285, row_height), step["title"], step["metric"] + "\n" + step["action"], "process", anchor)
        box(ax, (767, y, 332, row_height), step["output_title"], step["output"])
        arrow(ax, (352, y + row_height / 2), (417, y + row_height / 2))
        arrow(ax, (702, y + row_height / 2), (767, y + row_height / 2))
        if i + 1 < len(steps):
            arrow(ax, (559, y + row_height), (559, y + row_height + 45), dashed=True)
        y += row_height + 45
    return save(fig, destination, name, title + ". Horizontal arrows show data; dashed vertical arrows show execution order.")


def step(identifier, title, phase, inputs, action, outputs, effect, note, records, metric=None):
    duration = seconds(records, phase) if phase else None
    duration_label = f"{duration:.3f} s" if duration is not None and duration >= .001 else "<0.001 s"
    return {"id": identifier, "title": title, "phase": phase,
            "input_title": inputs[0], "input": inputs[1], "action": action,
            "output_title": outputs[0], "output": outputs[1], "effect": effect, "note": note,
            "metric": metric or (duration_label + " · SSE2 median" if phase else "Not timed separately")}


def overview(destination, records, annotation_records):
    fig, ax = canvas(1480)
    box(ax, (170, 20, 780, 88), "OBJ file · 7.109 GB", "Text: positions, UVs, corner references and group records", "input")
    box(ax, (370, 155, 380, 104), "A. Read, decode and merge", f"{seconds(records,'parser_total'):.2f} s · parallel parser workers", "process", "flow-parse")
    box(ax, (170, 306, 780, 118), "Parser output: P + U + C + F", "P: double XYZ · U: double UV · C: corner index triples · F: face/group metadata\nRGB values are checked and discarded; no supplied normals")
    box(ax, (335, 475, 450, 118), "B–D. Construct the CPU mesh", "Coordinate setup → triangle check → corner mapping → normals and whole-mesh bounds", "process", "flow-mapping")
    box(ax, (170, 646, 780, 136), "Shared CPU mesh: P + V + I + S + M", "V: 38.414 M render vertices · I: 227.316 M render indices\nS: corner → source position · M: render vertex → source position\nP is moved into a shared owner. Publication does not copy these arrays.")
    box(ax, (35, 858, 300, 118), "E. Group controls", f"{seconds(records,'group_state'):.2f} s\nCreate G: local/original bounds and display settings", "process", "flow-groups")
    box(ax, (410, 858, 300, 118), "F. Point preparation", f"{seconds(records,'scene_cpu_preparation'):.2f} s\nCreate L, Q, R and T for vertex overlays", "process", "flow-points")
    box(ax, (785, 858, 300, 118), "G. GPU upload", "Copy V, I, Q and R\n2.823 GB of buffer payload", "process", "flow-gpu")
    box(ax, (355, 1050, 520, 105), "Scene publication", "UI controls + shared CPU geometry + GPU handles\nSelect this box for the CPU/GPU memory diagrams.", url="flow-memory")
    box(ax, (80, 1230, 400, 118), "Draw frames", "GPU buffers + camera + group settings → colour/depth targets", "process", "flow-gpu")
    box(ax, (650, 1230, 400, 118), "H. Annotation worker", f"{seconds(annotation_records,'annotation_cache'):.2f} s · isolated run\nReads the same shared V + I + group ranges", "process", "flow-annotation")
    box(ax, (80, 1395, 400, 72), "Rendered image", "Screenshot is a separate readback.")
    box(ax, (650, 1395, 400, 72), "Annotation cache", "Blocks, fingerprints, offsets and tree")
    for start, end in (((560,108),(560,155)),((560,259),(560,306)),((560,424),(560,475)),((560,593),(560,646))):
        arrow(ax, start, end)
    for x, label in ((185,"V + I + P + M"),(560,"V + I + ranges"),(935,"V + I")):
        arrow(ax, (x,782), (x,858), label)
    arrow(ax, (335,917),(410,917),"G")
    arrow(ax, (710,917),(785,917),"Q + R")
    arrow(ax, (935,976),(740,1050),"GPU handles",via=[(935,1013),(740,1013)])
    arrow(ax, (185,976),(355,1103),"G: controls",via=[(185,1103)])
    arrow(ax, (500,1155),(280,1230),via=[(500,1192),(280,1192)])
    arrow(ax, (730,1155),(850,1230),"starts after publication",True,via=[(730,1192),(850,1192)])
    arrow(ax, (280,1348),(280,1395))
    arrow(ax, (850,1348),(850,1395))
    return save(fig,destination,"data-flow-overview","OBJ data flow from file bytes to shared CPU mesh, group controls, point data, GPU buffers, rendered frames and asynchronous annotation cache.")


def mapping_diagram(destination, records, diagnostic):
    counts = diagnostic["diagnostic_counts"]
    fig, ax = canvas(970)
    box(ax,(225,15,670,100),"C: 227.316 M corner references", "Each corner stores a position ID, UV ID and normal ID.\nThese are references to P and U, not render vertex IDs.","input")
    box(ax,(355,158,410,100),"Resolve the key (position, UV, normal)",f"All branches belong to corner_mapping: {seconds(records,'corner_mapping'):.2f} s", "process","flow-step-corner_mapping")
    arrow(ax,(560,115),(560,158))
    branches=[(20,"First use of this position",f'{counts["first_positions"]:,} corners\nCreate its primary render vertex.'),
              (402,"Same key as primary",f'{counts["primary_hits"]:,} corners\nReuse the existing render vertex.'),
              (784,"Different UV/normal key",f'{counts["secondary_lookups"]:,} lookups\nUse the secondary seam hash.')]
    for x,title,body in branches:
        box(ax,(x,330,316,120),title,body,"process")
        arrow(ax,(560,258),(x+158,330),via=[(560,295),(x+158,295)])
    box(ax,(20,520,660,120),"New vertex: read P and U", "Position: double XYZ → float XYZ\nUV: double UV → float UV, with v changed to 1 − v\nNormal: zero here; generated in the next stage", "input")
    box(ax,(760,520,340,120),"Seam result",f'{counts["secondary_hits"]:,} existing seam hits\n{diagnostic["secondary_vertices"]:,} new seam vertices')
    arrow(ax,(178,450),(180,520),"create")
    arrow(ax,(942,450),(930,520))
    arrow(ax,(760,555),(680,555),"new")
    box(ax,(225,720,670,112),"Outputs: V + I + S + M", "V: unique render vertices · I: render vertex ID for every corner\nS: source position ID for every corner\nM: source position ID for every render vertex")
    arrow(ax,(350,640),(400,720),"append V + M")
    arrow(ax,(560,450),(650,720),via=[(710,475),(710,680),(650,680)])
    ax.text(737,611,"reuse ID",rotation=90,ha="center",va="center",fontsize=11,color=MUTED)
    arrow(ax,(930,640),(800,720),"existing ID")
    ax.text(22,865,"Example of normalized IDs (illustrative; not records from this file):",fontsize=14,weight="bold",color=INK)
    ax.text(22,899,"Corners: (P7,U10,−1), (P7,U10,−1), (P7,U22,−1)  →  I = [0,0,1]  ·  S = [7,7,7]  ·  M = [7,7]",fontsize=12,color=INK)
    ax.text(22,930,"The two render vertices share one precise source position. A different UV creates the second vertex.",fontsize=12,color=MUTED)
    return save(fig,destination,"data-flow-corner-branches","Corner mapping splits render vertices at UV seams and creates render indices, source indices and render-to-source mappings.")


def memory_diagrams(destination, geometry, point_counts, annotation_counts, buffers):
    """Show live owners and copies, not process working-set or driver heap totals."""
    sizes = {identifier: size for identifier, _, _, _, size, _ in buffers}
    mesh_payload = sum(sizes[key] for key in ("P", "V", "I", "S", "M"))
    point_payload = sum(sizes[key] for key in ("L", "Q", "R", "T"))
    cpu_payload = mesh_payload + point_payload
    gpu_payload = sum(sizes[key] for key in ("V", "I", "Q", "R"))
    annotation_payload = sum(sizes[key] for key in ("B", "O", "H"))
    mb = lambda key: f"{sizes[key]/1e6:,.3f} MB"

    fig, ax = canvas(1575)
    ax.text(20, 18, "CPU RAM: retained structures at scene publication", fontsize=17,
            weight="bold", color=INK, va="top")
    box(ax, (20, 62, 1080, 106), "Known CPU array payload: " + f"{cpu_payload/1e9:.3f} GB ({cpu_payload/2**30:.3f} GiB)",
        f"Mesh arrays: {mesh_payload/1e9:.3f} GB populated; {geometry['retained_buffer_capacity_bytes']/1e9:.3f} GB allocated capacity.\nPoint arrays: {point_payload/1e6:,.3f} MB populated. Small metadata and spare capacity are additional.", "process")
    box(ax, (25, 218, 520, 188), "UiFileState.mesh.storage → MeshGeometry",
        f"V · vertices · {mb('V')}\nI · indices · {mb('I')}\nM · positionIndices · {mb('M')}\nMeshNode ranges plus sourcePositions → P")
    box(ax, (595, 218, 500, 188), "UiFileState.mesh.sourceData → SourceMeshData",
        f"S · indices · {mb('S')}\npositions → P\nS refers directly to precise source positions.\nNo second position array is retained.")
    box(ax, (170, 483, 780, 116), "P · one shared precise-position vector",
        f"{geometry['source_positions']:,} double XYZ records · {mb('P')}\nBoth sourcePositions pointers refer to this same allocation.\nRender vertex i uses P[M[i]]; add Mesh.origin for original coordinates.", "input")
    arrow(ax, (285, 406), (350, 483), "shared reference")
    arrow(ax, (845, 406), (780, 483), "shared reference")
    ax.text(570, 195, "I → V   ·   S → P   ·   M → P", ha="center", fontsize=11, color=MUTED)

    box(ax, (25, 655, 520, 158), "LoadedModelRuntime.gpuMesh (CPU struct)",
        "Graphics handles + GpuNodeRange records\npointCloud → shared const points::Cloud\npointChunks / proxyChunks hold GPU handles.\nThe handles refer to GPU allocations; their bytes are separate.")
    box(ax, (595, 655, 500, 158), "UiState.files[0] → UiFileState",
        "Mesh holds the geometry/source owners shown above.\nG · one UiGroupState: bounds, colour and transforms\nFile settings, origin and whole-mesh bounds\nThese CPU controls produce per-frame draw parameters.")

    cloud_frame = FancyBboxPatch((25, 900), 1070, 249,
        boxstyle="round,pad=0,rounding_size=9", facecolor="#f7f9fa", edgecolor="#cddadd", linewidth=1)
    ax.add_patch(cloud_frame)
    ax.text(45, 920, "points::Cloud · retained on the CPU, including after GPU upload", fontsize=14,
            weight="bold", color=INK, va="top")
    cloud_fields = [
        ("L · sourceVertices", f"{mb('L')}\nL[id - 1] → render ID\nThen M → P for precision"),
        ("Q · points", f"{mb('Q')}\nFloat XYZ + marker ID\nSpatially ordered originals"),
        ("R · proxies", f"{mb('R')}\nFloat XYZ + marker ID\nSamples from Q"),
        ("T · nodes", f"{mb('T')}\n{point_counts['nodes']:,} range/bounds nodes\nRanges select Q or R"),
    ]
    for i, (title, body) in enumerate(cloud_fields):
        box(ax, (45 + i*260, 958, 240, 133), title, body)
    ax.text(45, 1123, "Also retained: roots, group records and prepared navigation cuts. Q/R have additional GPU copies.", fontsize=11, color=MUTED)
    arrow(ax, (285, 813), (285, 900), "shared pointCloud owner")

    later = box(ax, (25, 1223, 1070, 192), "Later CPU addition: Mesh.annotationCache → MeshAnnotationCache",
        f"B · blocks · {mb('B')}   |   O · spatial triangle offsets · {mb('O')}   |   H · tree · {mb('H')}\n"
        f"B/O/H add {annotation_payload/1e6:,.3f} MB of populated payload after the background worker completes.\n"
        f"H alone has {annotation_counts['annotation_tree_build']['tree_capacity_bytes']/1e6:,.3f} MB of measured allocation capacity.\n"
        "The cache shares MeshGeometry above; it reads the same V/I without copying them.\n"
        "O refers to original triangle offsets in I. H/B store bounds and index ranges; group fingerprints are small.")
    later.set_edgecolor(MUTED)
    later.set_linestyle("--")
    later.set_linewidth(1)
    arrow(ax, (25, 1315), (25, 306), dashed=True, via=[(10, 1315), (10, 306)])
    ax.text(25, 1461, "Released construction data: U, C, parsed face records F, corner lookup tables, membership scratch and sort keys.",
            fontsize=11, color=MUTED)
    ax.text(25, 1490, "The OBJ text is not retained as a full text buffer. Windows file-cache pages are outside this model inventory.",
            fontsize=11, color=MUTED)
    ax.text(25, 1519, "Sizes are populated payloads. This diagram does not measure the complete process working set or upload staging.",
            fontsize=11, color=MUTED)
    cpu_svg = save(fig, destination, "memory-cpu", "CPU memory after scene publication: shared MeshGeometry, SourceMeshData, one shared precise-position vector, UiState controls, graphics handles and retained point cloud. The annotation cache arrives later.")

    fig, ax = canvas(1210)
    ax.text(20, 18, "GPU memory: model buffer copies after upload", fontsize=17,
            weight="bold", color=INK, va="top")
    box(ax, (20, 62, 1080, 87), f"Model buffer payload: {gpu_payload/1e9:.3f} GB ({gpu_payload/2**30:.3f} GiB)",
        "Four CPU streams are copied. This is buffer payload, not total GPU heap allocation or device-wide VRAM use.", "process")
    ax.text(20, 185, "CPU SOURCE · still retained", fontsize=11, color=MUTED)
    ax.text(450, 185, "GPU ALLOCATION · separate copy", fontsize=11, color=MUTED)
    chunk_size = 1048576
    gpu_fields = [
        ("V · Vertex array", "Float XYZ, normal and UV\n32 bytes per render vertex", "vertexBuffer · V copy",
         f"{geometry['vertices']:,} records · {mb('V')}\nPositions, normals and UVs used by the solid triangle draw."),
        ("I · triangle indices", "uint32 render vertex IDs\n4 bytes per triangle corner", "triangleIndexBuffer · I copy",
         f"{geometry['triangles']*3:,} IDs · {mb('I')}\nEach ID indexes the V copy above; three IDs form one triangle."),
        ("Q · Point array", "Float XYZ + marker ID\n16 bytes per original point", "pointChunks · Q copies",
         f"{point_counts['points']:,} points · {mb('Q')}\n{(point_counts['points']+chunk_size-1)//chunk_size} buffers, with up to 1,048,576 points per buffer. Used when vertex markers are drawn."),
        ("R · proxy Point array", "Float XYZ + marker ID\n16 bytes per sampled point", "proxyChunks · R copies",
         f"{point_counts['proxies']:,} proxies · {mb('R')}\n{(point_counts['proxies']+chunk_size-1)//chunk_size} buffers. CPU detail selection chooses ranges of Q or R; the tree stays on the CPU."),
    ]
    for i, (source_title, source_body, title, body) in enumerate(gpu_fields):
        y = 207 + i*198
        box(ax, (20, y, 300, 148), source_title, source_body, "input")
        box(ax, (450, y, 625, 148), title, body)
        arrow(ax, (320, y+74), (450, y+74), "copy")
    arrow(ax, (1075, 480), (1075, 281), via=[(1105, 480), (1105, 281)])
    ax.text(20, 1026, "Q/R are uploaded even while vertex markers are off. P, S, M, L, T and the annotation cache stay on the CPU.",
            fontsize=11, color=MUTED)
    box(ax, (20, 1066, 1080, 115), "Other graphics memory is outside the model-buffer total",
        "Colour/depth and MSAA render targets, programs, per-frame parameters, heaps and driver allocations also use graphics memory.\n"
        "Upload staging is transient or reusable; reclamation follows GPU completion fences.\n"
        "The recorded device-wide VRAM peak includes other applications and is not a per-buffer residency measurement.")
    gpu_svg = save(fig, destination, "memory-gpu", "GPU memory after model upload: separate copies of render vertices V, triangle indices I, original points Q and proxy points R, with copy arrows from the retained CPU buffers. Precise source data and CPU trees are not uploaded.")
    metadata = {"snapshot": "scene publication, plus separately labelled post-load annotation addition",
                "cpu_populated_array_bytes": cpu_payload, "cpu_mesh_populated_bytes": mesh_payload,
                "cpu_mesh_capacity_bytes": geometry["retained_buffer_capacity_bytes"],
                "cpu_point_populated_bytes": point_payload, "gpu_model_buffer_bytes": gpu_payload,
                "post_load_annotation_populated_bytes": annotation_payload,
                "annotation_tree_capacity_bytes": annotation_counts["annotation_tree_build"]["tree_capacity_bytes"],
                "cpu_arrays": ["P", "V", "I", "S", "M", "L", "Q", "R", "T"],
                "gpu_copies": ["V", "I", "Q", "R"], "annotation_arrays": ["B", "O", "H"],
                "element_bytes_windows_x64": {"PointNode": 76, "AnnotationBlock": 40, "AnnotationNode": 72},
                "excluded": "spare capacity (except the reported mesh capacity), small metadata, allocator/driver overhead, other applications, and transient upload storage"}
    return cpu_svg, gpu_svg, metadata


def write_data_flow(destination, data, records, viewer, upload):
    annotation_records = data["annotation"]["records"]
    geometry = records[0]["geometry"]
    point_counts = next(event["data"] for event in records[0]["events"] if event["name"] == "point_navigation_cuts")
    diagnostic = next(event["data"] for event in data["diagnostic"]["records"][0]["events"] if event["name"] == "corner_mapping")
    p, v, c = geometry["source_positions"], geometry["vertices"], geometry["triangles"]*3
    annotation_counts = {event["name"]: event["data"] for event in annotation_records[0]["events"]}
    uv_count = data["parser-scaling"]["records"][0]["geometry"]["texcoords"]
    buffers = [
        ("P","Precise source positions",p,"double XYZ; 24 bytes",p*24,"Moved, then shared by MeshGeometry and SourceMeshData; stays on the CPU."),
        ("U","Parsed UV coordinates",uv_count,"double UV; 16 bytes",uv_count*16,"Temporary; read by corner mapping and released when the importer returns."),
        ("C","Parsed corner references",c,"three 32-bit signed IDs; 12 bytes",c*12,"Temporary; replaced by I and S, then released."),
        ("F","Parsed face and group metadata",geometry["triangles"],"face sizes, material/smoothing IDs and group boundaries",None,"Temporary per-face records; final group ranges and names are copied into MeshNode metadata."),
        ("V","Render vertices",v,"float XYZ + normal + UV; 32 bytes",v*32,"CPU mesh retains V; GPU gets a copy. Normal generation updates this array in place."),
        ("I","Triangle render indices",c,"32-bit render vertex ID; 4 bytes",c*4,"CPU and GPU each retain this stream; refers to V."),
        ("S","Triangle source indices",c,"32-bit source position ID; 4 bytes",c*4,"Retained in SourceMeshData on the CPU; refers to P."),
        ("M","Render-to-source map",v,"32-bit source position ID; 4 bytes",v*4,"Retained on the CPU. Precise position of vertex i is P[M[i]]."),
        ("L","Overlay point ID-to-vertex map",v,"32-bit render vertex ID; 4 bytes",v*4,"Moved into Cloud.sourceVertices; retained for CPU picking and source identity."),
        ("Kp","Point sort keys",v,"Morton key + point ID; 8 bytes",v*8,"Temporary; radix sorting also uses a same-size scratch array."),
        ("Q","Spatially ordered point records",point_counts["points"],"float XYZ + original marker ID; 16 bytes",point_counts["points"]*16,"CPU and GPU retain Q; stored coordinates are not quantized."),
        ("R","Point proxy records",point_counts["proxies"],"same 16-byte Point layout",point_counts["proxies"]*16,"CPU and GPU retain R; sampled original points retain their IDs."),
        ("T","Point hierarchy",point_counts["nodes"],"bounds + ranges + child/proxy IDs; 76 bytes",point_counts["nodes"]*76,"CPU tree; guides detail selection, navigation and picking."),
        ("G","Logical group controls",geometry["groups"],"UiGroupState: bounds, display settings and transforms",None,"UiState retains logical controls on the CPU; runtime converts them to draw parameters."),
        ("B","Annotation triangle blocks",annotation_counts["annotation_cache"]["blocks"],"bounds + index range; 40 bytes; up to 256 triangles",annotation_counts["annotation_cache"]["blocks"]*40,"CPU annotation cache; geometry itself is shared."),
        ("Ka","Annotation centroid keys",geometry["triangles"],"Morton key + original triangle offset; 8 bytes",geometry["triangles"]*8,"Temporary; a same-size scratch array is used by radix sorting."),
        ("O","Spatial triangle offsets",geometry["triangles"],"32-bit offset into I; 4 bytes",geometry["triangles"]*4,"CPU annotation cache; preserves original triangle identity."),
        ("H","Annotation spatial tree",annotation_counts["annotation_tree_build"]["tree_nodes"],"bounds + child and triangle ranges; 72 bytes",annotation_counts["annotation_tree_build"]["tree_nodes"]*72,"CPU annotation cache; prunes triangle searches."),
    ]
    def make(identifier,title,phase,inputs,action,outputs,effect,note="",rs=None,metric=None):
        return step(identifier,title,phase,inputs,action,outputs,effect,note,rs or records,metric)
    chapters = [
      ("parse","A. Read, decode and merge", "The parallel scan produces local numeric chunks. Ordered collection and merge reconcile their references into global tables. The parser total includes these nested stages and other setup; do not add it to the child times.", [
        make("capacity_preflight","Check supported capacity","capacity_preflight",("File","File size and an optional bounded capacity scan"),"Check limits before large allocation.",("Continue to parsing","No early rejection; parsed counts are checked again later."),"Validate","The optional scan stops early when a sampled count is within capacity or an attribute/primitive record makes a full scan unnecessary. Process read counters report 1 MiB in the measured preflight."),
        make("parser_scan_collect","Read, decode and collect","parser_scan_collect",("File text","Buffered reads; 16 ranges; bounded raw text storage"),"Convert numbers and references; collect ranges in file order.",("Local numeric chunks","Positions, UVs, face references and metadata; reported chunk bytes: 4.555 GB"),"Create","RGB extras are decoded and validated, then discarded. No normals are supplied. Read and decode overlap."),
        make("parser_merge_shape_plan","Plan shape merge","parser_merge_shape_plan",("Collected chunks","Per-chunk counts and shape/group boundaries"),"Compute global offsets and final output sizes.",("Merge plan","Offsets, ranges and required array lengths"),"Create plan"),
        make("parser_attribute_allocation","Allocate merged arrays","parser_attribute_allocation",("Merge plan","Final attribute and primitive counts"),"Allocate destination arrays.",("Empty destinations","P, U, C and per-face/group metadata F"),"Allocate"),
        make("parser_merge_attribute_plan","Plan copy tasks","parser_merge_attribute_plan",("Chunks + destinations","Local arrays and global destination offsets"),"Create independent copy and index-adjustment tasks.",("Copy task list","Source/destination ranges and index bases"),"Create plan"),
        make("parser_merge_execute","Copy and reconcile indices","parser_merge_execute",("Local arrays + task list","Numeric chunks and primitive references"),"Copy attributes; convert local/relative indices to global references.",("P + U + C + F","37.891 M positions; 38.414 M UVs; 227.316 M corners; one group"),"Copy and update","Local chunk storage is temporary. Final output sizes in the buffer table are populated payloads, not the reported chunk-byte statistic."),
      ]),
      ("coordinates","B. Set coordinates and prepare construction buffers", "This model has triangles and UVs, with no analytic freeform patches, line primitives or explicit point primitives. A moved buffer changes owner without copying its elements.", [
        make("freeform_resolve","Resolve freeform records","freeform_resolve",("P + U + C + statements","Parsed geometry and ordered freeform records"),"Resolve any analytic control references.",("Resolved geometry","No freeform patches for this model; polygon data remains."),"Validate / resolve"),
        make("statement_release","Release statement records","statement_release",("Freeform statements + weights","Records consumed by freeform resolution"),"Drop numeric statement and sparse weight storage.",("Released temporary storage","P, U, C and F remain available."),"Release"),
        make("primitive_validation","Validate primitive ranges","primitive_validation",("C + F","Faces, line/point ranges and source counts"),"Check supported counts and primitive structure.",("Validated counts","75.772 M triangles; no line or point primitives"),"Validate"),
        make("source_position_adoption","Adopt source coordinates","source_position_adoption",("P: parser position vector","37.891 M double XYZ records"),"Move P into mesh-construction ownership.",("P: same elements","Ownership changes; no second XYZ array is copied."),"Move"),
        make("coordinate_origin","Choose coordinate origin","coordinate_origin",("P: file coordinates","Double XYZ values"),"Validate coordinates and select a local origin.",("Origin + P","Origin is (0,0,0) for this model; P remains."),"Create small value"),
        make("coordinate_rebase","Subtract the origin","coordinate_rebase",("P + origin","The same source position vector"),"Rewrite each point as P − origin.",("P: local coordinates","Same array, updated in place; zero origin here."),"Update in place"),
        make("triangulation","Inspect / triangulate faces","triangulation",("P + C + F","Position table and polygon corner lists"),"Split non-triangular polygons when required.",("Triangle C + F","Already triangular here; still checked. Each triangle has three corners."),"Validate / transform"),
        make("mesh_allocation","Reserve construction arrays","mesh_allocation",("P + source and corner counts","Source positions move into a shared owner."),"Reserve render buffers and lookup tables.",("Empty V + I + S + M","P is now shared; allocate primary entries, key storage and later seam buckets."),"Move, share, allocate","V and M reserve from source counts, then grow because UV seams add vertices."),
      ]),
      ("mapping","C. Convert corner references into render vertices", "One OBJ position can need several render vertices when its UV or normal differs between corners. The primary entry handles most corners; only alternate attribute keys use the seam hash. Branch counts below come from the diagnostic run, not separate timing intervals.", [
        make("corner_mapping","Resolve and write corners","corner_mapping",("P + U + C + lookup tables","Corner keys are (position ID, UV ID, normal ID)."),"Reuse a render ID or create a vertex; write both index streams.",("V + I + S + M + groups","38.414 M render vertices and two 227.316 M corner index streams"),"Create / append","V stores float positions, normals and flipped-v UVs. I addresses V; S and M address P. Lookup tables are temporary."),
      ]),
      ("normals","D. Generate normals, calculate bounds and publish", "Normal generation writes the normal field inside V; it does not create another full vertex array. Bounds passes create small results while reading large arrays. CPU publication creates a shared immutable owner, before the later scene-publication step in the viewer.", [
        make("normal_check","Check for complete normals","normal_check",("V.normal","Initial normal fields; all are zero for this input."),"Decide whether normals must be generated.",("Generation decision","Missing normals: generate them for this model."),"Validate"),
        make("normal_clear","Clear normal accumulators","normal_clear",("V","The constructed render vertex array"),"Set each vertex normal to zero.",("V: same array","Normal fields are ready for accumulation."),"Update in place"),
        make("normal_face_accumulation","Accumulate face normals","normal_face_accumulation",("V.position + I","Three indexed render vertices per triangle"),"Compute a unit face normal and add it to its three vertex normals.",("V.normal accumulators","Sums of adjacent unit face normals; 75.772 M triangles processed"),"Update in place","Face normals are transient local values; there is no retained array of all face normals."),
        make("normal_vertex_normalize","Normalize vertex sums","normal_vertex_normalize",("V.normal accumulators","One sum per render vertex"),"Normalize the accumulated vectors.",("V: completed normals","Same 32-byte vertex records, ready for shading"),"Update in place"),
        make("render_bounds","Calculate whole-mesh bounds","render_bounds",("V.position","38.414 M float XYZ records"),"Find min/max and centre; scan again for radius.",("Mesh.bounds","Small float bounds value; radius uses vertex distances."),"Create small value"),
        make("original_bounds","Calculate original bounds","original_bounds",("P + M + origin","For vertex i, fetch P[M[i]] and add the origin."),"Reduce precise coordinates to min/max.",("Mesh.originalBounds","Two double XYZ values; cached for whole-mesh use"),"Create small value"),
        make("mesh_publish","Publish CPU geometry","mesh_publish",("Draft V + I + M; shared P","Completed render buffers, group ranges and source data S"),"Validate storage and transfer its owner.",("Shared immutable mesh","The same arrays can be read by UI, upload and workers."),"Move / share","Publication does not copy the complete mesh. S remains in SourceMeshData; P has one shared owner."),
        make("parser_lookup_release","Release construction lookups","parser_lookup_release",("C + F + primary / key / bucket tables","No longer needed after render IDs are written."),"Destroy parsed polygons and lookup tables.",("Retained P + V + I + S + M","Temporary lookup storage is released."),"Release","U and the empty parsed normal array are released at importer return; that destruction is not a separately timed diagram step."),
      ]),
      ("groups","E. Create group controls and point membership", "Both group bounds read existing mesh arrays. Their results are small UiState values. Point membership lists the render vertices used by each group; these overlay points are different from explicit OBJ p records. Group creation completes before scene preparation starts.", [
        make("group_render_bounds","Calculate local group bounds","group_render_bounds",("V + I + group range","For this group, I contains all 227.316 M corners."),"Gather float positions and reduce group bounds.",("G.localBounds","Min/max, centre and box-based radius; no new V array"),"Read again / create small value"),
        make("group_original_bounds","Calculate original group bounds","group_original_bounds",("I + M + P + origin","Corner → render ID → source ID → precise XYZ"),"Reduce original-frame coordinates for this group.",("G.originalBounds","Two double XYZ values; G also retains display settings."),"Read again / create small value","The group pass traverses corners rather than all unique render vertices. It repeats geometry already covered by whole-mesh bounds."),
        make("gpu_index_validation","Validate upload indices","gpu_index_validation",("V + I + group ranges","Published immutable mesh; display feature flags"),"Check capacities, index bounds and valid triangle ranges.",("Validated upload input","Same mesh arrays; no new geometry"),"Validate"),
        make("point_membership_allocate","Allocate membership buffers","point_membership_allocate",("V count + group count","38.414 M render vertices; one group"),"Reserve L and initialize temporary group markers.",("Empty L + membership scratch","L uses 32-bit vertex IDs; scratch uses size_t per vertex."),"Allocate"),
        make("point_membership_build","Collect unique group vertices","point_membership_build",("I + group ranges + scratch","Walk corners; mark vertices already added to this group."),"Add each referenced render vertex once per group.",("L + GPU node ranges","38.414 M point-to-vertex entries for the one group"),"Create","Membership scratch is released when scene preparation returns. No edge-index buffer is created with the measured default features."),
      ]),
      ("points","F. Build the point hierarchy", "L moves into Cloud.sourceVertices and keeps the original marker-to-render mapping. Morton keys put nearby positions near each other in a sorted list. The quantized key changes ordering only; Q and R store unquantized float positions. The hierarchy is built even while vertex markers are off.", [
        make("point_range_bounds","Calculate point range bounds","point_range_bounds",("L + V.position + group ranges","Use each marker ID to find its render position."),"Validate positions and calculate group min/max.",("Point group bounds + L","Small bounds used to normalize spatial keys"),"Create small value"),
        make("point_morton_keys","Create spatial sort keys","point_morton_keys",("Point bounds + L + V.position","38.414 M marker positions"),"Interleave 10-bit coordinates into a Morton key.",("Kp: (key, marker ID)","8 bytes per point; ID retains the original mapping."),"Create temporary array"),
        make("point_radix_sort","Sort point keys","point_radix_sort",("Kp","Key array plus a same-size temporary scratch array"),"Run four stable 8-bit radix passes.",("Kp: spatial order","Positions have not been rewritten or quantized."),"Update / temporary allocation"),
        make("point_sorted_gather","Gather sorted point records","point_sorted_gather",("Sorted Kp + L + V.position","Each marker ID still identifies its render vertex."),"Copy positions and IDs into Q; release keys.",("Q + retained L","38.414 M records; Q is 614.630 MB, L is 153.658 MB."),"Create / release"),
        make("point_tree_build","Build tree and proxy samples","point_tree_build",("Q","Spatially ordered original points with persistent IDs"),"Split ranges, reduce bounds and sample original points.",("T + R + group roots",f'{point_counts["nodes"]:,} nodes; {point_counts["proxies"]:,} proxy points; Q remains.'),"Create","Proxy records are sampled original points, not averaged replacements. R occupies 70.077 MB."),
        make("point_navigation_cuts","Prepare navigation cuts","point_navigation_cuts",("T + R + group roots","Tree ranges and proxy counts"),"Choose camera-independent cuts for several budgets.",("Cloud + prepared cuts","Q, R, T, L and group metadata; ready for upload and picking"),"Create small selection lists"),
      ]),
      ("gpu","G. Copy selected buffers to the GPU and draw", "The GPU receives copies of V, I, Q and R. Precise P, source indices S, mappings M/L and CPU trees are not in this upload payload. Q/R are allocated on the GPU even though vertex markers are off; the measured solid draw uses V/I. The upload measurement is summed submission work, not its paced wall interval.", [
        make("gpu_upload","Upload selected buffers",None,("V + I + Q + R","CPU arrays plus prepared node ranges"),"Submit incremental buffer creation/copies to the graphics backend.",("GPU buffer copies","V: 1,229.261 MB; I: 909.264 MB; Q + R: 684.707 MB"),"Copy CPU → GPU",metric=f'{float(upload[2])/1000:.3f} s · submission steps'),
        make("scene_publication","Publish the viewer scene",None,("GPU handles + shared mesh + G","CPU scene metadata and completed upload job"),"Install the loaded file into the viewer scene.",("Visible scene","CPU geometry stays shared; graphics handles stay in runtime state."),"Publish",metric=f'{viewer["load_rpc_ms"]/1000:.2f} s · complete load endpoint'),
        make("gpu_render","Draw the model",None,("GPU V + I; camera and G","Triangle vertex/index buffers, matrices and appearance"),"Run vertex and fragment work and present the frame.",("Colour/depth targets","A rendered frame; screenshot capture adds readback."),"Create per-frame pixels",metric=f'{statistics.median(e["gpuFrameMilliseconds"] for e in viewer["frames"]["events"]):.2f} ms · median GPU frame'),
      ]),
      ("annotation","H. Prepare annotations on a background worker", "This branch starts after scene publication. Its cache holds a shared owner of the input mesh, so it does not copy V or I. B, O and H are new derived CPU data. Spatial offsets preserve the original triangle identities even though their order changes.", [
        make("annotation_blocks_fingerprints","Build blocks and fingerprints","annotation_blocks_fingerprints",("Shared V + I + group ranges","The same immutable mesh used by the viewer"),"Reduce bounds in blocks and hash group geometry.",("B + group fingerprints","295,985 blocks, up to 256 triangles each; one group fingerprint"),"Create derived cache",rs=annotation_records),
        make("annotation_morton_keys","Create triangle centroid keys","annotation_morton_keys",("B + V + I","Three indexed positions for each triangle"),"Find each centroid and encode its spatial order.",("Ka: (key, original offset)","75.772 M pairs; 606.176 MB of populated key payload"),"Create temporary array",rs=annotation_records),
        make("annotation_radix_sort","Sort keys and extract offsets","annotation_radix_sort",("Ka + scratch","Same-size scratch supports four radix passes."),"Sort centroid keys, then keep original triangle offsets.",("O: offsets into I","75.772 M offsets; 303.088 MB. Keys and scratch are released."),"Create / release",rs=annotation_records),
        make("annotation_tree_build","Build triangle search tree","annotation_tree_build",("O + shared V + I","Spatial order and the original triangle geometry"),"Build leaf bounds and combine child bounds.",("H + annotation cache","2,097,151 nodes; cache contains B, fingerprints, O/H and a shared geometry owner."),"Create / share",rs=annotation_records),
      ]),
    ]
    controls = [
        make("control_read_cached","Cached read control",None,("OBJ file through Windows cache","Disjoint ranges; synchronous 256 KiB reads"),"Read all bytes without parsing.",("Read buffers + byte count","7,108,991,827 bytes verified; no geometry produced"),"Measurement only",metric=f'{seconds([r for r in data["io"]["records"] if r["mode"]=="read-cached" and r["workers_requested"]==8],"read_only"):.3f} s · 8-worker median'),
        make("control_read_native","Native read control",None,("OBJ file from input device","Aligned reads bypass the Windows file cache."),"Read all bytes without parsing.",("Read buffers + byte count","Physical-device read control; not cold production parsing"),"Measurement only",metric=f'{seconds([r for r in data["io"]["records"] if r["mode"]=="read-native" and r["workers_requested"]==8],"read_only"):.3f} s · 8-worker median'),
        make("control_copy","Memory copy control",None,("Initialized source buffer","512 MiB; separate 512 MiB destination"),"Copy source to destination 16 times.",("Destination bytes + throughput","Contiguous copy calibration; no mesh or indices"),"Measurement only",metric=f'{seconds(data["memory"]["records"],"memory_copy"):.3f} s · median'),
        make("geometry_validation","Probe geometry validation","geometry_validation",("P + V + I + M","Read positions, UVs, indices and normals after timing."),"Calculate geometry and normal fingerprints.",("Two integer fingerprints","Verify compared outputs; no new mesh geometry"),"Measurement only","This extra pass is excluded from production load and CPU preparation totals."),
    ]
    chapters.append(("controls","I. Measurement controls, outside production loading","These controls are separate fresh-process experiments. They do not pass data to the real mesh-construction pipeline. Their vertical order is for reading the diagram, not a production dependency.",controls))
    overview_svg = overview(destination, records, annotation_records)
    mapping_svg = mapping_diagram(destination, records, diagnostic)
    cpu_memory_svg, gpu_memory_svg, memory_metadata = memory_diagrams(destination, geometry, point_counts, annotation_counts, buffers)
    memory_content = ('<details class="flow-chapter" id="flow-memory"><summary>End-of-load memory: CPU and GPU structures</summary>'
        '<p>These diagrams show model data owned at scene publication. The CPU keeps its arrays after uploading GPU copies. The annotation cache is a later CPU addition. All MB/GB labels use decimal units; GiB uses 2³⁰ bytes.</p>'
        '<p>Populated payload is count × element size. The measured mesh allocation capacity includes unused vector space. Neither total is the complete process working set or total VRAM allocation. Point-node and annotation-record sizes were checked against the Windows x64 Release compiler.</p>'
        '<h3 id="flow-memory-cpu">CPU RAM: owners and retained structures</h3><figure>'+cpu_memory_svg+'</figure>'
        '<p class="small"><a href="memory-cpu.svg">CPU SVG</a> · <a href="memory-cpu.png">CPU PNG</a></p>'
        '<h3 id="flow-memory-gpu">GPU memory: copies and buffer relationships</h3><figure>'+gpu_memory_svg+'</figure>'
        '<p class="small"><a href="memory-gpu.svg">GPU SVG</a> · <a href="memory-gpu.png">GPU PNG</a></p></details>')
    fragments = []
    for identifier,title,intro,steps in chapters:
        diagram = rows_diagram(destination,"data-flow-"+identifier,title,steps)
        detail = ''.join('<details id="flow-step-'+html.escape(s["id"])+ '"><summary>'+html.escape(s["title"])+
                         ' · '+html.escape(s["effect"])+ '</summary><p>'+html.escape(s["note"] or s["action"])+
                         '</p><p class="small">Measured phase: <code>'+html.escape(s["phase"] or "viewer / control")+
                         '</code>. '+html.escape(s["metric"])+'.</p></details>' for s in steps)
        branch = mapping_svg if identifier == "mapping" else ""
        fragments.append('<details class="flow-chapter" id="flow-'+identifier+'"><summary>'+html.escape(title)+
                         '</summary><p>'+html.escape(intro)+'</p><figure>'+branch+diagram+'</figure>'+detail+
                         '<p class="small"><a href="data-flow-'+identifier+'.svg">SVG diagram</a> · <a href="data-flow-'+identifier+'.png">PNG diagram</a></p></details>')
    catalog_rows = ''.join('<tr>'+''.join('<td>'+html.escape(str(cell))+'</td>' for cell in
                          (identifier,name,f'{count:,}',layout,"Not tabulated" if size is None else f'{size/1e6:,.3f} MB',life))+'</tr>'
                          for identifier,name,count,layout,size,life in buffers)
    nav = '<nav class="flow-nav" aria-label="Data-flow chapters">'+''.join('<a href="#flow-'+identifier+'">'+html.escape(title.split('. ',1)[0])+': '+html.escape(title.split('. ',1)[1])+'</a>' for identifier,title,_,_ in chapters)+'<a href="#flow-memory">End-of-load CPU/GPU memory</a></nav>'
    content = ('<section id="data-flow"><h2>Data flow: what each step reads and creates</h2><p>Follow the arrows from file text to pixels. Letter IDs identify the same buffers across diagrams. Horizontal arrows in each detailed map show inputs and outputs; dashed vertical arrows show execution order. In the overview, forks show data reuse and do not imply that the branches run in parallel. Only the annotation branch starts on a separate worker after scene publication.</p>'+nav+overview_svg+
              '<p class="small">Select a process box to open its detailed map. Input buffers use a pale orange fill; process steps use teal; outputs use a pale neutral fill. Box size does not encode byte size. <a href="data-flow-overview.svg">Overview SVG</a> · <a href="data-flow-overview.png">Overview PNG</a></p>'+memory_content+''.join(fragments)+
              '<details id="flow-buffers"><summary>Buffer IDs, sizes and lifetime</summary><p>Sizes below are populated payloads calculated from measured counts and C++ element layouts. They exclude unused vector capacity, allocator overhead and temporary scratch unless stated. A CPU buffer and its GPU copy are distinct allocations. P is shared by CPU owners, not copied for each owner.</p><table><thead><tr><th>ID</th><th>Data</th><th>Records</th><th>Layout</th><th>Payload</th><th>Lifetime and consumer</th></tr></thead><tbody>'+catalog_rows+'</tbody></table></details></section>')
    metadata = {"note":"Document diagrams; not new measurements. Payload bytes are count × C++ element size.",
                "memory_snapshot": memory_metadata,
                "chapters":[{"id":identifier,"title":title,"steps":steps} for identifier,title,_,steps in chapters],
                "buffers":[{"id":identifier,"name":name,"records":count,"layout":layout,"payload_bytes":size,"lifetime":life}
                           for identifier,name,count,layout,size,life in buffers]}
    (destination/"data-flow.json").write_text(json.dumps(metadata,indent=2),encoding="utf-8")
    return content


FLOW_STYLE = """.flow-nav{display:flex;flex-wrap:wrap;gap:8px 18px;margin:20px 0}.flow-nav a{font-size:14px}.flow-chapter>summary,#flow-buffers>summary{font-size:18px;font-weight:600;padding:12px 0}.flow-chapter figure{margin:16px 0}.flow-chapter svg{display:block;margin:12px 0}.flow-chapter details{margin:12px 0}.flow-chapter details summary{font-size:14px}#flow-buffers table{font-size:13px}html{scroll-behavior:smooth}@media(prefers-reduced-motion:reduce){html{scroll-behavior:auto}}"""
FLOW_SCRIPT = """<script>
function openFlowTarget(){
 const id=decodeURIComponent(location.hash.slice(1));
 const target=document.getElementById(id);
 if(!target)return;
 for(let node=target;node;node=node.parentElement){if(node.tagName==='DETAILS')node.open=true;}
 requestAnimationFrame(()=>target.scrollIntoView({block:'start'}));
}
addEventListener('hashchange',openFlowTarget);
addEventListener('load',()=>{if(location.hash)openFlowTarget();});
</script>"""

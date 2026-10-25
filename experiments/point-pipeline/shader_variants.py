"""Build isolated shader audit candidates. Production sources are never edited.

Keep the source and binary directories from the same reference Release build.
Each output contains its complete sources, commands, hashes, and SPIR-V.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def replace(source, before, after):
    if before not in source:
        raise ValueError(f"Source anchor missing: {before[:100]}")
    return source.replace(before, after)


def shared_sample_source(points):
    points = replace(points, "float2 samplePosition(uint sample) {\n    if (cloud.samples==1) return float2(.5,.5);",
        "float2 pointSamplePosition4(uint sample) {")
    start = points.index("[ForceInline] void rasterProjectedPoint(")
    middle = points.index("[ForceInline] void rasterPoints(", start)
    end = points.index("#ifdef WOBY_POINT_ATOMIC64\n[numthreads", middle)
    core = points[start:end].replace("rasterProjectedPoint(", "WOBY_RASTER_POINT(")
    core = core.replace("rasterPoints(", "WOBY_RASTER_TASK(").replace("cloud.samples", "WOBY_RASTER_SAMPLES")
    core = core.replace("samplePosition(sample)", "WOBY_RASTER_SAMPLE_POSITION(sample)")
    core = ("// Intentionally included twice. The parent defines the function names,\n"
        "// literal sample count, and sample positions for each specialization.\n"+core)
    dispatch = ("// The adaptor accepts only 1x or 4x MSAA. Compile the shared raster body\n"
        "// with a literal sample count, then choose once per task. This removes\n"
        "// sample-count branches and address arithmetic from the inner loops.\n")
    for count in (1,4):
        position = "float2(.5,.5)" if count==1 else "pointSamplePosition4(sample)"
        dispatch += (f"#define WOBY_RASTER_SAMPLES {count}u\n"
            f"#define WOBY_RASTER_POINT rasterProjectedPoint{count}\n"
            f"#define WOBY_RASTER_TASK rasterPointTask{count}\n"
            f"#define WOBY_RASTER_SAMPLE_POSITION(sample) {position}\n"
            '#include "opaque_point_raster.slang"\n'
            "#undef WOBY_RASTER_SAMPLES\n#undef WOBY_RASTER_POINT\n#undef WOBY_RASTER_TASK\n#undef WOBY_RASTER_SAMPLE_POSITION\n")
    dispatch += ("[ForceInline] void rasterProjectedPoint(float4 clip,float pointSize,uint id,bool query,bool ids) {\n"
        "    if (cloud.samples==1) rasterProjectedPoint1(clip,pointSize,id,query,ids);\n"
        "    else rasterProjectedPoint4(clip,pointSize,id,query,ids);\n}\n"
        "[ForceInline] void rasterPoints(uint3 dispatch,uint lane,bool ids) {\n"
        "    if (cloud.samples==1) rasterPointTask1(dispatch,lane,ids);\n"
        "    else rasterPointTask4(dispatch,lane,ids);\n}\n")
    return points[:start]+dispatch+points[end:], core


def variant(name, mesh, points):
    flags = []
    if name == "split-conditional":
        mesh, points, _ = variant("conditional-uv", mesh, points)
        return variant("split-vertices", mesh, points)
    if name in ("baseline-normalized", "sample-normalized"):
        return variant("baseline" if name=="baseline-normalized" else "sample-generic", mesh, points)
    if name in ("threads32", "threads64", "threads256", "threads512", "threads1024", "threads256-opt3", "threads512-opt3", "threads1024-opt3"):
        width = name.split("-")[0].removeprefix("threads")
        points = replace(points, "i+=128", f"i+={width}")
        points = replace(points, "numthreads(128,1,1)", f"numthreads({width},1,1)")
        if name.endswith("-opt3"): flags = ["-O3"]
    elif name == "direct-atomics":
        points = replace(points, "if (((Atomic<uint64>*)address)->load(MemoryOrder.Relaxed)<value)", "")
    elif name == "direct-certificate":
        points = replace(points, "pointAtomicMax64(cloud.coverage+tileIndex,key);",
            "InterlockedMax(cloud.coverage[tileIndex],key);")
    elif name == "depth32-certificate":
        points = replace(points,
            "if (((Atomic<uint64>*)(cloud.coverage+tileIndex))->load(MemoryOrder.Relaxed)>=key) continue;",
            "if (((Atomic<uint>*)((uint*)cloud.coverage+tileIndex*2+1))->load(MemoryOrder.Relaxed)>depth) continue;")
        points = replace(points, "pointAtomicMax64(cloud.coverage+tileIndex,key);",
            "InterlockedMax(((uint*)cloud.coverage)[tileIndex*2+1],depth);")
    elif name == "wave-atomics":
        points = replace(points, "// Winners only increase during a raster dispatch.",
            "if (WaveActiveAllEqual(uint64(address))) {\n            value=WaveActiveMax(value);\n            if (!WaveIsFirstLane()) return;\n        }\n        // Winners only increase during a raster dispatch.")
    elif name == "untiled-fastpath":
        start = points.index("            for (int y=start.y;")
        end = points.index("\n        }\n}\n[ForceInline]", start)
        loop = points[start:end]
        points = replace(points, "int tileSize=int(1u<<cloud.tileShift);",
            "if (cloud.tileSize==0) {\n            int2 start=lo,finish=hi;\n" + loop +
            "\n            return;\n        }\n        int tileSize=int(1u<<cloud.tileShift);")
    elif name == "block-cull":
        helper = '''groupshared uint rejectBlock;
bool blockOutside(uint3 dispatch) {
    uint taskIndex=dispatch.y*cloud.dispatchWidth+dispatch.x;
    if (taskIndex>=cloud.taskCount) return true;
    OpaquePointGroupData group=cloud.groups[cloud.tasks[taskIndex].group];
    float* bounds=(float*)cloud.batch;
    uint b=taskIndex*6;
    float3 low=float3(bounds[b],bounds[b+1],bounds[b+2]);
    float3 high=float3(bounds[b+3],bounds[b+4],bounds[b+5]);
    float4 magnitude=mul(abs(group.matrix),float4(max(abs(low),abs(high)),1));
    float4 error=16.0*1.1920928955078125e-7*magnitude;
    if (!all(isfinite(error))) return false;
    uint outside=63;
    for (uint corner=0;corner<8;++corner) {
        float3 p=float3((corner&1)!=0?high.x:low.x,(corner&2)!=0?high.y:low.y,(corner&4)!=0?high.z:low.z);
        float4 clip=mul(group.matrix,float4(p,1));
        if (!all(isfinite(clip)) || clip.w<=error.w) return false;
        float2 pad=group.pointSize/float2(cloud.width,cloud.height)*(clip.w+error.w);
        uint planes=(clip.x < -clip.w-pad.x-error.x-error.w?1u:0u)
            |(clip.x > clip.w+pad.x+error.x+error.w?2u:0u)
            |(clip.y < -clip.w-pad.y-error.y-error.w?4u:0u)
            |(clip.y > clip.w+pad.y+error.y+error.w?8u:0u)
            |(clip.z < -error.z?16u:0u)|(clip.z > clip.w+error.z+error.w?32u:0u);
        outside&=planes;
        if (outside==0) return false;
    }
    return outside!=0;
}
'''
        entry = "[numthreads(128,1,1)] void cs_opaque_raster(uint3 dispatch:SV_GroupID,uint lane:SV_GroupIndex) {\n    rasterPoints(dispatch,lane,false);"
        points = replace(points, entry, helper + "[numthreads(128,1,1)] void cs_opaque_raster(uint3 dispatch:SV_GroupID,uint lane:SV_GroupIndex) {\n    if (lane==0) rejectBlock=blockOutside(dispatch)?1u:0u;\n    GroupMemoryBarrierWithGroupSync();\n    if (rejectBlock!=0) return;\n    rasterPoints(dispatch,lane,false);")
    elif name == "sample-mask":
        points = replace(points,
            "[unroll] for (uint sample=0;sample<4;++sample) {\n                if (sample>=cloud.samples) break;\n                float2 delta=float2(x,y)+samplePosition(sample)-center;\n                if (dot(delta,delta)>radius*radius) continue;",
            "{\n                uint mask=0;\n                [unroll] for (uint s=0;s<4;++s) {\n                    if (s>=cloud.samples) break;\n                    float2 delta=float2(x,y)+samplePosition(s)-center;\n                    if (dot(delta,delta)<=radius*radius) mask|=1u<<s;\n                }\n                while (mask!=0) {\n                uint sample=firstbitlow(mask); mask&=mask-1;")
        points = replace(points, "#endif\n            }\n        }\n}", "#endif\n                }\n            }\n        }\n}")
    elif name == "pixel-interior":
        points = replace(points,
            "[unroll] for (uint sample=0;sample<4;++sample) {\n                if (sample>=cloud.samples) break;",
            "{\n                float2 farthest=max(abs(float2(x,y)+(cloud.samples==1?.5:.125)-center),abs(float2(x,y)+(cloud.samples==1?.5:.875)-center));\n                bool interior=dot(farthest,farthest)<=radius*radius;\n                [unroll] for (uint sample=0;sample<4;++sample) {\n                if (sample>=cloud.samples) break;")
        points = replace(points, "if (dot(delta,delta)>radius*radius) continue;", "if (!interior && dot(delta,delta)>radius*radius) continue;")
        points = replace(points, "#endif\n            }\n        }\n}", "#endif\n                }\n            }\n        }\n}")
    elif name == "clear-tiles":
        points = replace(points, "if (cloud.coverage!=nullptr) {", "if (cloud.coverage!=nullptr && cloud.tileSize!=0) {")
        points = replace(points, "cloud.coverage[index]=uint64(lowerBound)<<32;",
            "uint columns=(cloud.width+cloud.tileSize-1)>>cloud.tileShift;\n        uint rows=(cloud.height+cloud.tileSize-1)>>cloud.tileShift;\n        if (index<columns*rows) cloud.coverage[index]=uint64(lowerBound)<<32;")
    elif name == "single-group":
        points = replace(points, "while (lo<hi)", "while (cloud.groupCount>1 && lo<hi)")
    elif name == "sample-index":
        # Compute loops bound sample before calculating an address. Keep the
        # fragment resolve clamp: helper lanes require that separate guard.
        start = points.index("struct ResolveVertex")
        points = points[:start].replace("min(sample,cloud.samples-1)", "sample") + points[start:]
    elif name == "sample-count":
        start = points.index("[ForceInline] void rasterProjectedPoint(")
        end = points.index("#ifdef WOBY_POINT_ATOMIC64\n[numthreads", start)
        projected = points[start:points.index("[ForceInline] void rasterPoints(", start)]
        raster = points[points.index("[ForceInline] void rasterPoints(", start):end]
        specialized = []
        for count in (1, 4):
            body = projected.replace("rasterProjectedPoint(", f"rasterProjectedPoint{count}(")
            body = body.replace("cloud.samples", f"{count}u")
            body = body.replace("samplePosition(sample)", "float2(.5,.5)" if count==1 else "auditSamplePosition4(sample)")
            specialized.append(body)
            specialized.append(raster.replace("rasterPoints(", f"rasterPoints{count}(")
                .replace("rasterProjectedPoint(", f"rasterProjectedPoint{count}("))
        specialized.insert(0, "float2 auditSamplePosition4(uint sample) {\n"
            "    const float2 positions[4]={float2(.375,.125),float2(.875,.375),float2(.125,.625),float2(.625,.875)};\n"
            "    return positions[sample];\n}\n")
        specialized.append("[ForceInline] void rasterPoints(uint3 dispatch,uint lane,bool ids) {\n"
            "    if (cloud.samples==1) rasterPoints1(dispatch,lane,ids);\n"
            "    else rasterPoints4(dispatch,lane,ids);\n}\n")
        points = points[:start] + "".join(specialized) + points[end:]
    elif name == "sample-lanes":
        points = replace(points, "uint id,bool query,bool ids) {", "uint id,bool query,bool ids,uint ownedSample) {")
        points = replace(points, "for (uint i=lane;i<task.count;i+=128)",
            "for (uint i=lane/cloud.samples;i<task.count;i+=128/cloud.samples)")
        points = replace(points, "task.query!=0,ids);", "task.query!=0,ids,lane%cloud.samples);")
        points = replace(points, "[unroll] for (uint sample=0;sample<4;++sample)",
            "for (uint sample=ownedSample;sample<ownedSample+1;++sample)")
        # All sample lanes for a point must survive its own certificate. A
        # strictly greater certificate can replace all of them together.
        points = replace(points, "load(MemoryOrder.Relaxed)>=key) continue;", "load(MemoryOrder.Relaxed)>key) continue;")
    elif name == "sample-shared":
        points, _ = shared_sample_source(points)
    elif name in ("sample-generic", "sample-query"):
        start = points.index("float2 samplePosition(")
        end = points.index("#ifdef WOBY_VULKAN", start)
        helper = points[start:end].replace("samplePosition(", "samplePosition<let samples : uint>(")
        helper = helper.replace("cloud.samples", "samples")
        points = points[:start]+helper+points[end:]
        start = points.index("[ForceInline] void rasterProjectedPoint(")
        middle = points.index("[ForceInline] void rasterPoints(", start)
        end = points.index("#ifdef WOBY_POINT_ATOMIC64\n[numthreads", middle)
        projected = points[start:middle].replace("rasterProjectedPoint(", "rasterProjectedPointSamples<let samples : uint>(")
        projected = projected.replace("cloud.samples", "samples").replace("samplePosition(sample)", "samplePosition<samples>(sample)")
        raster = points[middle:end].replace("rasterPoints(", "rasterPointTaskSamples<let samples : uint>(")
        raster = raster.replace("rasterProjectedPoint(", "rasterProjectedPointSamples<samples>(")
        if name == "sample-query":
            projected = projected.replace("<let samples : uint>", "<let samples : uint, let query : bool>")
            projected = projected.replace("bool query,bool ids", "bool ids")
            raster = raster.replace("<let samples : uint>", "<let samples : uint, let query : bool>")
            raster = raster.replace("<samples>(", "<samples,query>(").replace("task.query!=0,ids", "ids")
        wrappers = "[ForceInline] void rasterProjectedPoint(float4 clip,float pointSize,uint id,bool query,bool ids) {\n"
        for count in (1,4):
            wrappers += ("    if (cloud.samples==1) {\n" if count==1 else "    else {\n")
            if name == "sample-query":
                wrappers += f"        if (query) rasterProjectedPointSamples<{count},true>(clip,pointSize,id,ids);\n"
                wrappers += f"        else rasterProjectedPointSamples<{count},false>(clip,pointSize,id,ids);\n"
            else: wrappers += f"        rasterProjectedPointSamples<{count}>(clip,pointSize,id,query,ids);\n"
            wrappers += "    }\n"
        wrappers += "}\n[ForceInline] void rasterPoints(uint3 dispatch,uint lane,bool ids) {\n"
        if name == "sample-query":
            wrappers += "    uint taskIndex=dispatch.y*cloud.dispatchWidth+dispatch.x;\n    if (taskIndex>=cloud.taskCount) return;\n"
            wrappers += "    bool query=cloud.tasks[taskIndex].query!=0;\n"
        for count in (1,4):
            wrappers += ("    if (cloud.samples==1) {\n" if count==1 else "    else {\n")
            if name == "sample-query":
                wrappers += f"        if (query) rasterPointTaskSamples<{count},true>(dispatch,lane,ids);\n"
                wrappers += f"        else rasterPointTaskSamples<{count},false>(dispatch,lane,ids);\n"
            else: wrappers += f"        rasterPointTaskSamples<{count}>(dispatch,lane,ids);\n"
            wrappers += "    }\n"
        wrappers += "}\n"
        points = points[:start]+projected+raster+wrappers+points[end:]
    elif name in ("lossless-points", "lossless21-points"):
        points = replace(points, "for (uint i=lane;i<task.count;i+=128) {\n        OpaquePoint p=task.points[i];",
            "uint4 encoding=((uint4*)cloud.batch)[taskIndex];\n"
            "    for (uint i=lane;i<task.count;i+=128) {\n"
            "        OpaquePoint p;\n"
            "        if (encoding.w!=4) {\n"
            "            uint* words=(uint*)task.points+i*3;\n"
            "            uint xy=words[0];\n"
            "            uint2 delta=uint2(xy&65535,xy>>16);\n"
            "            uint3 bits=encoding.w==1?uint3(words[1],encoding.yz+delta)\n"
            "                :encoding.w==2?uint3(encoding.x+delta.x,words[1],encoding.z+delta.y)\n"
            "                :uint3(encoding.xy+delta,words[1]);\n"
            "            p.position=asfloat(bits);\n"
            "            p.id=words[2];\n"
            "        } else p=task.points[i];")
        if name == "lossless21-points":
            start = points.index("            uint2 delta=uint2(xy&65535,xy>>16);")
            end = points.index("            p.position=asfloat(bits);", start)
            points = (points[:start]+"            uint z=words[1];\n"
                "            uint3 bits=encoding.xyz+uint3(xy&0x1fffff,(xy>>21)|((z&0x3ff)<<11),z>>10);\n"
                +points[end:])
    elif name == "conditional-uv":
        mesh = replace(mesh, "v.uv = float2(root.vertices[p+6],root.vertices[p+7]);",
            "v.uv = root.uvGrid.z > .5 ? float2(root.vertices[p+6],root.vertices[p+7]) : float2(0);")
        # Comparison attributes are independent of the source UV-grid state.
        start = mesh.index("VertexOutput vs_comparison")
        mesh = mesh[:start] + mesh[start:].replace(
            "v.uv = root.uvGrid.z > .5 ? float2(root.vertices[p+6],root.vertices[p+7]) : float2(0);",
            "v.uv = float2(root.vertices[p+6],root.vertices[p+7]);")
    elif name == "identity-normal":
        mesh = replace(mesh,
            "v.normal = mul(root.model,float4(root.vertices[p+3],root.vertices[p+4],root.vertices[p+5],0)).xyz;",
            "v.normal = float3(root.vertices[p+3],root.vertices[p+4],root.vertices[p+5]);")
    elif name == "early-tests":
        # Audit opaque fills only. A production version would need separate
        # entries so hidden-line interiors and zero-alpha draws keep their depth.
        for entry in ("fs_marker_mesh", "fs_marker_mesh_edges", "fs_marker_mesh_edges_pulled"):
            mesh = replace(mesh, "MarkerOutput " + entry + "(", "[earlydepthstencil] MarkerOutput " + entry + "(")
    elif name == "split-vertices":
        mesh = replace(mesh, "float2(root.vertices[p+6],root.vertices[p+7])",
            "float2(root.analysisFaces[index*2],root.analysisFaces[index*2+1])")
        mesh = replace(mesh, "root.vertices[corner*root.stride+6+metric%2]", "root.analysisFaces[corner*2+metric%2]")
    elif name == "edge-square":
        mesh = replace(mesh,
            "float3 distances = bary / max(sqrt(dx*dx+dy*dy),float3(1e-8));\n    float distance = min(distances.x,min(distances.y,distances.z));",
            "float3 distances = sign(bary)*bary*bary / max(dx*dx+dy*dy,float3(1e-16));\n    float distance = sqrt(max(0.0,min(distances.x,min(distances.y,distances.z))));")
    elif name == "circle-dot":
        mesh = replace(mesh, "length(v.uv) > .5", "dot(v.uv,v.uv) > .25")
    elif name == "line-fastclip":
        mesh = replace(mesh, "for (uint i=0;i<6;++i) {",
            "bool inside=all(abs(a.xy)<=a.w) && all(abs(b.xy)<=b.w) && a.z>=0 && a.z<=a.w && b.z>=0 && b.z<=b.w;\n    if (!inside) for (uint i=0;i<6;++i) {")
    elif name == "lookup-prune":
        mesh = replace(mesh, "if (distance>9) continue;", "if (distance>9 || distance>bestDistance) continue;")
    elif name == "cube-weight":
        mesh = replace(mesh,
            "float weight = clamp(pow(min(1.0,color.a*10)+.01,3)*pow(1-depth*.9,3),1e-4,1.0);",
            "float a=min(1.0,color.a*10)+.01, b=1-depth*.9;\n    float weight = clamp((a*a*a)*(b*b*b),1e-4,1.0);")
    elif name in ("opt2", "opt3"):
        flags = ["-O" + name[-1]]
    elif name == "baseline":
        pass
    else:
        raise ValueError(f"Unknown variant: {name}")
    return mesh, points, flags


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference_source", type=Path)
    parser.add_argument("reference_shaders", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--ngapi", type=Path, required=True)
    parser.add_argument("--validator", type=Path, required=True)
    parser.add_argument("--variants", nargs="+", default=["threads64", "threads256", "sample-mask", "pixel-interior",
        "clear-tiles", "single-group", "conditional-uv", "edge-square", "circle-dot", "cube-weight", "opt3"])
    args = parser.parse_args()
    source, reference, output, compiler, ngapi, validator = (p.resolve() for p in
        (args.reference_source, args.reference_shaders, args.output, args.compiler, args.ngapi, args.validator))
    output.mkdir(parents=True, exist_ok=False)
    mesh = (source/"woby.slang").read_text()
    points = (source/"opaque_points.slang").read_text()
    for name in args.variants:
        destination = output/name
        shutil.copytree(reference, destination/"spirv")
        shutil.copytree(source, destination/"source")
        changed_mesh, changed_points, flags = variant(name, mesh, points)
        (destination/"source/woby.slang").write_text(changed_mesh)
        (destination/"source/opaque_points.slang").write_text(changed_points)
        if name == "sample-shared":
            (destination/"source/opaque_point_raster.slang").write_text(shared_sample_source(points)[1])
        commands = []
        with (destination/"compile.log").open("w") as log:
            for binary in sorted((destination/"spirv").glob("*.bin")):
                entry = binary.stem
                point = "_opaque_" in entry
                if not flags and (changed_points == points if point else changed_mesh == mesh):
                    continue
                command = [str(compiler), str(destination/"source"/("opaque_points.slang" if point else "woby.slang")),
                    "-target", "spirv", "-profile", "spirv_1_5", "-emit-spirv-directly", "-fvk-use-entrypoint-name",
                    "-DWOBY_VULKAN", "-warnings-as-errors", "all", "-entry", entry,
                    "-stage", "compute" if entry.startswith("cs_") else "vertex" if entry.startswith("vs_") else "fragment",
                    "-fvk-use-c-layout", "-matrix-layout-row-major", "-I", str(ngapi/"include"),
                    "-I", str(ngapi/"utility/include"), "-capability", "spvDescriptorHeapEXT", *flags, "-o", str(binary)]
                if entry == "cs_opaque_raster": command.append("-DWOBY_POINT_ATOMIC64")
                if name == "wave-atomics" and entry == "cs_opaque_raster":
                    for capability in ("spvGroupNonUniform", "spvGroupNonUniformArithmetic", "spvGroupNonUniformVote"):
                        command += ["-capability", capability]
                if entry in ("fs_mesh_edges", "fs_marker_mesh_edges", "fs_transparent_edges"):
                    command += ["-capability", "spvFragmentBarycentricKHR"]
                commands.append(command)
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
                subprocess.run([str(validator), "--target-env", "vulkan1.4", "--scalar-block-layout", str(binary)],
                    stdout=log, stderr=subprocess.STDOUT, check=True)
            if name in ("baseline-normalized", "sample-normalized"):
                optimizer=validator.with_name("spirv-opt"+validator.suffix)
                for entry in ("cs_opaque_raster", "cs_opaque_depth", "cs_opaque_ids"):
                    binary=destination/"spirv"/(entry+".bin")
                    temporary=binary.with_suffix(".optimized")
                    command=[str(optimizer),"--strip-debug","--compact-ids",str(binary),"-o",str(temporary)]
                    commands.append(command)
                    subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
                    temporary.replace(binary)
                    subprocess.run([str(validator),"--target-env","vulkan1.4","--scalar-block-layout",str(binary)],
                        stdout=log,stderr=subprocess.STDOUT,check=True)
        report = dict(commands=commands, hashes={str(p.relative_to(destination)):hashlib.sha256(p.read_bytes()).hexdigest()
            for p in destination.rglob("*") if p.is_file()})
        (destination/"manifest.json").write_text(json.dumps(report, indent=2))
        print(f"{name}: {len(commands)} compiler/optimizer commands; generated binaries validated", flush=True)


if __name__ == "__main__":
    main()

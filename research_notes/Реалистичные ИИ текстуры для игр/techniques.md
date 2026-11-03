# Techniques for crisp, realistic detail in AI-generated game surface textures (2023-2026)

Scope note: about 15 searches and fetches. Primary sources were found for tiling and blending, mip and downscale filters, detail maps, PBR estimation and tile ControlNet. Evidence on prompting ("macro", cm-per-image) and on upscaler quality for textures is thin and mostly vendor or enthusiast material; it is flagged below. Claims without a source are in "Inferences" or "Gaps", not "Cited Findings".

## 1. Prompting for macro and micro detail (real scale, "macro photograph", "scanned texture", camera terms)

### Takeaway
I found no controlled evidence that scale or camera words ("macro", "scanned", "high-frequency detail", "X cm across") reliably sharpen gpt-image, Flux or SDXL texture output. What people actually use are texture LoRAs with trigger words, plus negative prompts against blur and objects, and careful sampler/CFG tuning. Prompts also do not carry over between model generations.

### Cited Findings
- SDXL texture workflow (A1111): prompt "top-down image of rough solid flat dark, rich slate rock" with emphasis brackets, and the negative prompt `blurry, cracks, crevice, round, smooth, distinct, pebble, shadows, coin, circle`. Settings: Euler, 60 steps, 1024², CFG 6.5 ("especially important"), Tiling on, Hires fix off ("poor results in every configuration"). About 50 images in 2 h, many bad. Prompts that worked on SD1 did not transfer to SDXL — [cprimozic.net, "Generating textures for 3D using Stable Diffusion"](https://cprimozic.net/notes/posts/generating-textures-for-3d-using-stable-diffusion/)
- Turning on tiling noticeably changes what the model generates, so you cannot match hosted services that lack a tiling option — [cprimozic.net](https://cprimozic.net/notes/posts/generating-textures-for-3d-using-stable-diffusion/)
- Flux texture LoRAs exist with trigger words: "smlstxtr" + "seamless texture" (gokaygokay Flux-Seamless-Texture-LoRA; licence differs between the model card, Apache-2.0, and Civitai, FLUX.1-dev non-commercial) — [HF model card](https://huggingface.co/gokaygokay/Flux-Seamless-Texture-LoRA), [Civitai](https://civitai.com/models/900955/flux-seamless-texture-lora). There is a macro-texture LoRA with trigger "MCROTX" — [fofr/flux-macro-texture](https://huggingface.co/fofr/flux-macro-texture). Scenario sells a commercial "Realistic Textures 3.0" Flux LoRA for stone, metal, wood and fabric — [Scenario](https://www.scenario.com/models/realistic-textures-30)
- A macro-photography Flux LoRA guide recommends "macro photography", "close-up", "sharp focus" and focusing on fine texture details, but it targets nature close-ups, not tileable materials — [NightCafe](https://creator.nightcafe.studio/model/IA4x9BZX0L74XfenkRic)

### Inferences
- The SDXL negative prompt ("round, pebble, coin, circle, distinct") suggests a big part of "soapiness" comes from the model drawing *objects* (big blobs) instead of a fine stochastic surface. Prompts that describe surface statistics (grain, flecks, pores) and push away object nouns are likely more useful than generic "high detail" words.
- With a hosted model (gpt-image) you cannot add LoRAs, so the levers are the prompt, the reference image, and post-processing (sections 2-5). Saying the real field of view ("a 40 cm square of …") is cheap to try, but treat it as a hypothesis to A/B test, not a known fix.

### Gaps
- No source measured whether "scanned texture", "photogrammetry", "cm per image" or camera/lens terms raise high-frequency energy in gpt-image, Flux or SDXL. A small in-house A/B test (e.g. high-pass energy or spectrum slope of outputs) would be needed.
- No source specific to gpt-image texture prompting was found.

## 2. Generating at real scale: small patch at full resolution, then tiling or composing; stochastic tiling, Wang/hex tiles, histogram-preserving blending

### Takeaway
A well-established route: generate or obtain a small, seamless, stochastic patch at high pixel density, then hide the repetition at runtime or bake time with hex-tile, histogram-preserving blending (Heitz & Neyret 2018; Burley 2019; Mikkelsen 2022). An alternative route makes a bigger texture by stitching several similar seamless variants (4× area) instead of upscaling.

### Cited Findings
- Heitz & Neyret, "High-Performance By-Example Noise using a Histogram-Preserving Blending Operator" (HPG/ACM 2018, Unity + Inria): from one stochastic exemplar it synthesises infinite non-repeating output. Texture space sits on a triangular lattice, a hexagonal tile with a random offset sits at each vertex, and each sample blends 3 tiles. Plain linear blending loses contrast, so the histogram is Gaussianised before blending and inverted after; the transforms are precomputed so it runs in a fragment shader — [Crossref record](https://api.crossref.org/works/10.1145%2F3233304); description in [Mikkelsen, JCGT 2022](https://jcgt.org/published/0011/03/05/paper.pdf)
- Limitation: it preserves only first-order statistics (the histogram), not higher-order structure, so it suits stochastic textures (plaster, dirt, stone grain) better than structured ones (bricks, planks) — [SciTePress 2021](https://www.scitepress.org/Papers/2021/102046/102046.pdf)
- Burley (JCGT 2019) removes the long precomputation with per-channel 1D lookup tables — [JCGT 0008/04/02](https://www.jcgt.org/published/0008/04/02/paper.pdf). Mikkelsen, "Practical Real-Time Hex-Tiling" (JCGT 2022), simplifies tile compositing and is the practical version — [JCGT 0011/03/05](https://jcgt.org/published/0011/03/05/paper.pdf)
- Open demo code for stochastic texture sampling — [GitHub HybridDog/stochastic_texture_sampling_demo](https://github.com/HybridDog/stochastic_texture_sampling_demo/)
- Composing instead of upscaling: take 4 similar seamless textures (variants made with img2img variation strength 0.1-0.3) and blend them with a "seamless stitcher" into one seamless texture at 4× area. It works badly when the sources differ in style or colour. Hex tiling is suggested for non-repeating terrain — [cprimozic.net](https://cprimozic.net/notes/posts/generating-textures-for-3d-using-stable-diffusion/) (tool: texture-utils.ameo.design/seamless-stitcher)
- Seams in generated materials: noise unrolling plus Poisson solving at the borders give tileability. Above 1024 px, patches are stitched and seams can show unless handled — [Lopes et al., Material Palette Extraction, CVPR 2024 suppl.](https://openaccess.thecvf.com/content/CVPR2024/supplemental/Lopes_Material_Palette_Extraction_CVPR_2024_supplemental.pdf)
- TexTile (CVPR 2024): a differentiable tileability metric (ConvNeXt + linear self-attention classifier). It can serve as an extra loss for diffusion texture synthesis, and existing perceptual metrics disagree on tileability — [arXiv 2403.12961](https://arxiv.org/abs/2403.12961v1), [CVPR paper](https://openaccess.thecvf.com/content/CVPR2024/papers/Rodriguez-Pardo_TexTile_A_Differentiable_Metric_for_Texture_Tileability_CVPR_2024_paper.pdf). A non-peer-reviewed thesis on hexagonal tileable textures with a pretrained LDM reports TexTile rising from 0.437 to 0.581 — [ITB thesis](https://informatika.stei.itb.ac.id/~rinaldi.munir/TA/Makalah-TA-Dhaffin.pdf)
- Tiling mode in SD can lose clarity at large sizes, so an upscaler is often needed afterwards — [aiarty guide](https://aiarty.com/stable-diffusion-guide/stable-diffusion-tiling.htm) (vendor blog, low authority)

### Inferences
- Pixel density is the core issue. A 1024 px picture of a whole 4 m wall gives about 4 mm per pixel *at best*, and image models rarely draw real detail at that density. A 1024 px picture of a 40-50 cm patch gives about 0.4-0.5 mm per pixel. That patch, downscaled to the in-game texel density, keeps real micro-contrast. For a 512-1024 px game texture over a large surface, a hybrid is natural: an AI "layout" image for the unique large-scale features (stains, cracks, timber layout) plus a small high-density stochastic patch for grain (section 4, frequency separation).
- Histogram-preserving hex blending can be done offline in numpy as a *bake* (no shader needed). This fits an engine where adding a custom shader is costly. Gaussianise per channel via the CDF, blend 3 hex tiles with variance-preserving weights (w/√Σw²), then invert the CDF.
- Wang tiles: not covered by the sources found. They remain a known option for structured textures, but no 2023-2026 practitioner source was retrieved.

### Gaps
- No source compares "one big image of a whole wall" against "patch + tiling" directly on perceived sharpness.
- Wang-tile sources for AI textures were not retrieved.

## 3. Upscalers: Real-ESRGAN, 4x-UltraSharp and other openmodeldb models, SUPIR, Magnific, Topaz, Krea

### Takeaway
Generative upscalers (Magnific Creative, SUPIR, tile-ControlNet SD) *invent* detail. That helps soapy images, but it causes pattern drift, mutated structure and broken seams, and no independent test on game textures was found. Faithful modes (Magnific Precision, tile ControlNet with low denoise) add little real detail. OpenModelDB has texture-specific 1× models (PBRify) for normal and height maps rather than albedo.

### Cited Findings
- OpenModelDB PBRify_Remix collection: models trained on CC0 textures from ambientCG and Poly Haven to upscale low-quality game textures and generate PBR maps (aimed at RTX Remix). The height model is "best used after applying a delighting pipeline". NormalV3 is CC0-1.0 — [openmodeldb PBRify collection](https://openmodeldb.info/collections/c-pbrify), [1x-PBRify-Height](https://openmodeldb.info/models/1x-PBRify-Height), [1x-PBRify-NormalV3](https://openmodeldb.info/models/1x-PBRify-NormalV3)
- A Baldur's Gate 3 upscale mod upscales only albedo maps and leaves normal and physical maps alone, because AI upscaling of those "typically introduces lighting errors without real in-engine gains" — [Nexus Mods BG3 #18252](https://www.nexusmods.com/baldursgate3/mods/18252) (modder opinion)
- Magnific: Creative mode adds new detail. A third-party comparison says it can drift, mutate patterns and redraw text into nonsense; Precision mode is for content that must stay the same — [Magnific blog on modes](https://www.magnific.com/blog/understanding-freepiks-upscaler-modes/), [invideo comparison](https://invideo.io/blog/ai-image-upscalers/). Magnific's API docs list "video game assets: upscale textures" as a use — [Magnific API docs](https://docs.magnific.com/api-reference/image-upscaler-creative/image-upscaler). Scenario docs say Creative fits concept art where "stylistic evolution is desired over literal fidelity" — [Scenario help](https://help.scenario.com/articles/8823252319-magnific-upscaling-the-essentials)
- Scenario's seamless-texture enhancer offers 2×/4× with Precise/Balanced/Creative presets and advises: "always check the edges after enhancing" for seam continuity — [Scenario: Upscale and Enhance Seamless Textures](https://help.scenario.com/enhance-seamless-textures)
- SUPIR is built on SDXL (2.6 B parameters). Claims that it beats Magnific and stays "almost 100%" faithful come from an enthusiast tutorial, not a benchmark; it needs about 12 GB VRAM per that tutorial — [HF blog post](https://huggingface.co/blog/MonsterMMORPG/supir-sota-image-upscale-better-than-magnific-ai), [DEV tutorial](https://dev.to/furkangozukara/supir-full-tutorial-1-click-12gb-vram-windows-runpod-linux-installer-batch-upscale-comparison-with-magnific-1nnc)
- One practitioner used *no* upscaler at all to get from 1K to 4K and stitched variants instead (section 2) — [cprimozic.net](https://cprimozic.net/notes/posts/generating-textures-for-3d-using-stable-diffusion/)

### Inferences
- For a 512-1024 px *final* texture, upscaling is mostly useful as an intermediate: upscale ×4 → manually or algorithmically pick and transfer detail → downscale with a good filter. The "detail" a GAN upscaler like Real-ESRGAN adds tends to be its own learned sharpening/texture prior, which can read as a uniform "AI crunch" across different materials.
- Upscaling a seamless texture should be done on a wrapped (padded with its own opposite edges) image and cropped back, or seams will appear. This follows from Scenario's warning to check edges.

### Gaps
- No independent, asset-level comparison of Real-ESRGAN, 4x-UltraSharp, SUPIR, Magnific, Topaz or Krea on tileable game textures was found.
- 4x-UltraSharp, Topaz and Krea pages were not retrieved in this pass.

## 4. img2img / ControlNet (tile, depth, normal), IP-Adapter, inpainting; detail transfer, high-pass overlay, frequency separation; engine detail maps

### Takeaway
Tile ControlNet with tiled diffusion is the standard open-source way to "re-detail" while keeping layout. Its trade-off is direct: the more it keeps the layout, the less detail it adds. Frequency separation (low frequencies from your own layout and colour, high frequencies from a photo, scan or generated patch) is a well-understood, deterministic alternative that is easy in numpy. Engines handle close-up detail with a separate, high-tiling, mean-neutral detail map.

### Cited Findings
- r/StableDiffusion (Jul 2024): tile ControlNet + Tiled Diffusion as a "very realistic upscaler". It works well on realistic images and less well on stylised ones; the tile ControlNet "is keeping the model from hallucinating, but also from adding or changing too much" — [Reddit mirror](https://lr.in.psf.lt/r/StableDiffusion/s/OjR7qqwbFu)
- xinsir ControlNet Tile SDXL: diffusers example with the SDXL base and the madebyollin fp16-fix VAE (recommended when using other base models) — [HF model card](https://huggingface.co/xinsir/controlnet-tile-sdxl-1.0). A HF demo combines MoD tiled diffusion with SDXL tile ControlNet — [HF space app.py](https://huggingface.co/spaces/MartsoBodziu1994/mod-control-tile-upscaler-sdxl/blob/main/app.py)
- Frequency separation splits an image into a low-frequency layer (colour and tone) and a high-frequency layer (fine texture). O3DE documents using a high-pass of an image as the terrain detail map (Photoshop Filter > Other > High Pass, GIMP, or Substance Designer) — [O3DE: Understanding Frequency Separation](https://docs.o3de.org/docs/learning-guide/tutorials/environments/understanding-frequency-separation/) (summary taken from the search snippet; the full page body was not parsed)
- Photo-retouching practice: a strong blur for the low layer (about 6 px is one guide's example, tuned until no colour bleeds into the detail layer). For high-pass radius, start small and raise it; "very easy to go too far" — [CreativeLive lesson](https://site.creativelive.com/class/skin-retouching-kristina-sherk/lessons/frequency-separation), [Adobe community thread](https://community.adobe.com/t5/photoshop-ecosystem-discussions/sharpening-using-high-pass-filter/m-p/15436837)
- Unity HDRP Detail Map: channel-packed texture (R = desaturated albedo, G = normal Y, B = smoothness, A = normal X), with its own UV set or tiling, masked by the blue channel of the Mask Map — [Unity HDRP docs](https://docs.unity3d.com/Packages/hdrp/manual/Mask-Map-and-Detail-Map.html). Detail maps are normally tiled "quite high" across the surface — [Unity discussions](https://discussions.unity.com/t/are-detail-masks-and-detail-maps-the-same-thing/1664884)
- Legacy Unity: a detail texture is a small fine pattern faded in as you approach. "It's very important that the image is centered around 128", otherwise the surface brightens or darkens with distance — [Unity 4.6 manual](https://docs.unity3d.com/462/Documentation/Manual/HOWTO-UseDetailTexture.html). UE2/UDK detail texture: a modifier layer shown only up close, default Detail Scale 8 — [UDK docs](https://docs.unrealengine.com/udk/Two/DetailTexture.html)

### Inferences
- A numpy recipe for "our layout + real grain":
  1. Work in linear light.
  2. `low = gaussian(ours, σ)`, with σ set to the scale below which our image is "soap" (for example the AI image's blur radius after downscaling to the target).
  3. `high = scan - gaussian(scan, σ')`, from a CC0 scan or a high-density generated patch resampled to the same mm/px; scale it to match the original's high-frequency standard deviation.
  4. Result = `low + k·high` (or multiplicative `low·(1+k·high/mean)` to keep dark crevices dark).

  This keeps colour and composition while replacing missing micro-contrast. Pitfall: if the high band carries strong directional features (cracks, mortar lines) they will fight the layout's large features. Mask by material region or pick a non-directional band.
- If the engine supports a detail texture (Gothic/ZenGin has no standard one; OpenGothic would need shader work), a mean-128 high-pass grain tiled at 4-8× gives up-close crispness without enlarging the base texture. Otherwise bake the high band into the base texture before mip generation.
- IP-Adapter (style or texture reference into SD) and depth/normal ControlNets were not covered by retrieved sources. They are plausible for "keep geometry from our Blender bake, take material look from a photo".

### Gaps
- No retrieved source on IP-Adapter or depth/normal ControlNet specifically for game-texture detail.
- The O3DE page body (blend modes, neutral value) was not parsed; only its summary snippet is used.

## 5. Downscaling correctly: Lanczos vs box, sharpening after downscale, mip generation (Kaiser, sharpened mips), why textures blur in game (mips, anisotropic filtering, LOD bias)

### Takeaway
Use a high-quality windowed-sinc filter (Lanczos3) for the downscale to the target size. Box or triangle averaging is the default in many tools and is the softest reasonable choice; Kaiser in Unity and NVTT is also called "too blurry". For the mip chain, use light per-level unsharp masking (OIIO `--sharpen 0.25-0.5`). Remember that in-game softness is often anisotropic filtering or mip selection, not the texture itself. Keep LOD bias at most about −0.5.

### Cited Findings
- OpenImageIO maketx: mip filters include `lanczos3`, `box`, `triangle` (default, which equals box for even sizes, i.e. a 2×2 average), `catrom`, `blackman-harris`, `gaussian`, `mitchell`, `bspline`, `sinc` and others. `lanczos3` is recommended "for the highest-quality filtering" but has negative lobes that ring on HDR values above 1 (use `--hicomp`). `--sharpen <contrast>` (experimental) applies an unsharp mask per mip level; suggested 0.5 or 0.25, and 1.0 may cause artefacts at high mips. `--filter unsharp-median` gives a median-based variant that stresses small features without over-stressing big edges — [OIIO maketx docs](https://openimageio.readthedocs.io/en/v3.1.16.0/_sources/maketx.rst.txt)
- Unity offers only Box and Kaiser mip filters, which users describe as producing blurred mips. An NVIDIA forum poster: "Box is too sharp and aliased, Kaiser is too blurry" — [Unity discussion](https://discussions.unity.com/t/feature-request-point-filtering-for-mipmaps-generation/1738210), [NVIDIA forum](https://forums.developer.nvidia.com/t/sharpening-effect-to-mip-levels/341674), [Unity TextureImporterMipFilter](https://docs.unity3d.com/kr/2019.4/ScriptReference/TextureImporterMipFilter.html)
- An NVTT bug made Kaiser mips "a blurry mess" when parameters were set on the command line (both parameters wrote to one slot); fixed in NVIDIA Texture Tools 2024.1.1 — [NVIDIA forum](https://forums.developer.nvidia.com/t/nvtte-2024-1-0-mip-filter-parameters-not-read-from-command-line/291915)
- Unreal workaround: import one size larger, MipGen "Sharpen10" (or lower) and LOD bias 1. It can make "really splotchy" lower mips — [Unreal forum "Crisp Pixels"](https://forums.unrealengine.com/t/crisp-pixels/22553/6). Filament ships its own mipgen tool — [Filament mipgen](https://www.mintlify.com/google/filament/tools/mipgen)
- LOD bias: positive bias blurs, negative sharpens; large negative bias costs performance, so don't go below about −0.5. Anisotropic filtering is "in most cases" the better way to sharpen oblique surfaces — [Unity Texture.mipMapBias](https://docs.unity3d.com/520/Documentation/ScriptReference/Texture-mipMapBias.html). An anecdote: a −4 bias shipped by a game caused heavy aliasing — [overtake.gg thread](https://www.overtake.gg/threads/negative-lod-bias.10513/post-505622)

### Inferences
- Walls seen at grazing angles (most building facades from the street) lose detail mainly to mip selection along the compressed axis. Check that the game runs 16× anisotropic filtering before blaming the texture. In OpenGothic this is a renderer and sampler setting, worth verifying in code.
- A practical offline chain: master at 2-4× the target → downscale with Lanczos3 in linear light → optional mild unsharp (radius about 0.5-1 px, small amount) at the *target* size → generate mips yourself with Lanczos or Kaiser plus `--sharpen 0.25`. This keeps high-frequency contrast that box averaging and soft Kaiser lose.
- Box-downscaling a soapy AI image does not create detail. Downscaling only helps when the source actually has detail at the scale that becomes one target texel. Hence section 2: pixel density at generation time matters more than the filter.
- Gamma: averaging in sRGB darkens fine bright detail and shifts contrast. Downscale in linear light (not sourced here, standard practice).

### Gaps
- No retrieved source quantifies Lanczos against box on texture sharpness, or Kaiser parameters (alpha, width) that give a crisp result.
- Whether OpenGothic generates mips at load, with which filter, and which anisotropy level it requests was not researched (a codebase question).

## 6. PBR map generation from an AI albedo; delighting

### Takeaway
Diffusion-based estimators (Marigold normals, depth and intrinsics/IID, ETH Zürich, CVPR 2024 and v1.1/v2) can produce normals, albedo, shading, roughness and metallic from one image, which makes them useful both for delighting an AI albedo and for normals. Classic or specialised tools (Materialize, SmartNormap, Poly, PBRify, Substance B2M) do height and normal from luminance. Delighting before height and normal is repeatedly recommended.

### Cited Findings
- Marigold Normals v1.1 is a diffusion normal estimator; authors recommend 1-4 steps for a speed/quality trade-off — [Replicate marigold-normalsv2](https://replicate.com/jasonod888/marigold-normalsv2). A Replicate wrapper combines Marigold normals with intrinsics (albedo / shading / residual), about $0.029 a run and under 30 s, and is runnable locally in Docker — [Replicate marigold-normals-intrinsics](https://replicate.com/jasonod888/marigold-normals-intrinsics)
- Marigold IID Appearance v1.1 extracts albedo, roughness and metallicity; IID Lighting v1.1 extracts diffuse shading and lighting residual for cleaner albedo separation — [Floyo "Texture to PBR" workflow](https://www.floyo.ai/workflows/texture-to-pbr-image-to-material-b2nju5t8qfh5)
- Original Marigold (CVPR 2024) repurposed Stable Diffusion. Marigold V2 models are built on a diffusion transformer and ship as LoRAs for depth, normals and albedo, with ComfyUI nodes — [ComfyUI docs, Marigold V2](https://docs.comfy.org/tutorials/utility/marigold-v2.md)
- PBRify height model: use after delighting — [openmodeldb 1x-PBRify-Height](https://openmodeldb.info/models/1x-PBRify-Height)
- Poly (withpoly.com) generates normal, height, AO, metalness and roughness; normal and height are free, the rest is $20/mo. SmartNormap is a non-AI normal generator, judged weaker than Poly — [cprimozic.net](https://cprimozic.net/notes/posts/generating-textures-for-3d-using-stable-diffusion/)
- Upscaling or regenerating normal and physical maps with AI "typically introduces lighting errors without real in-engine gains" (modder opinion) — [Nexus Mods BG3](https://www.nexusmods.com/baldursgate3/mods/18252)

### Inferences
- AI albedos have baked-in lighting (shadows in crevices, top-light gradients). This lighting is the main reason derived height and normal maps go wrong and why the surface looks "painted". Run delighting (Marigold IID Lighting, or a large-radius high-pass/flatten in numpy for low-frequency gradients) first, then derive normal and height.
- For a Gothic-era engine with no PBR, the main use is (a) a delighted albedo whose micro-contrast comes from material, not fake light, and (b) a normal map, if the renderer uses one, designed at a plausible strength. This matches the project's own prior lesson that tile normals were about 4× too strong.
- GPU needs: Marigold, SUPIR (about 12 GB VRAM) and SDXL tile ControlNet all need a CUDA GPU, which fits `dev-ubuntu` (RTX 3090) or the hosted Replicate route, not a MacBook. Frequency separation, hex blending, Lanczos/unsharp and mip generation are pure CPU numpy/OIIO.

### Gaps
- StableNormal, DeepBump, Materialize and Substance Sampler / B2M details were not retrieved in this pass; no sourced comparison of their quality on AI albedos.
- No source evaluates delighting quality specifically on AI-generated (rather than photographed) textures.

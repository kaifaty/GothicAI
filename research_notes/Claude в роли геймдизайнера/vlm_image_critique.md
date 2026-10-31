# Rigorous image critique with vision-language models (Claude vision first, GPT and Gemini too): techniques, documented limits, prompt patterns

Scope note: the research was done on 2026-10-21. Most academic findings come from the GPT-4V / GPT-4o / Gemini 1.x–2.5 / Claude 3.x era (2024–2025). Anthropic's own docs describe current Claude models up to Opus 5.5. Anthropic itself says to re-test scaffolding built for older models (see Q1). Each finding is tagged with the model generation it was measured on.

## Q1. What does Anthropic document about Claude's vision capabilities and limits, and how does it recommend prompting image tasks?

### Takeaway
Claude sees an image as a grid of 28×28-pixel patches, capped per model. Opus/Sonnet 4.7 and later take up to 2576 px on the long edge and 4784 visual tokens; older models take 1568 px / 1568 tokens. Detail below patch size is lost, and Anthropic says no amount of prompting recovers it. The documented limits are counting, approximate localization, small, rotated or low-quality images, and AI-image detection. The fix Anthropic recommends is tooling, not prompting: put images before text, label them `Image 1:`/`Image 2:`, ask for absolute pixel coordinates, and give Claude a crop/zoom tool or a container with PIL/OpenCV so it can "crop, zoom, measure, and verify its work."

### Cited Findings
**How images are processed and what they cost (current Claude models)**
- Each patch is 28×28 px and counts as one visual token. Cost = `⌈width/28⌉ × ⌈height/28⌉` tokens — [Anthropic Vision guide](https://platform.claude.com/docs/en/build-with-claude/vision)
- Resolution tiers:
  - High-resolution tier, "Claude 4.7 and later models": max long edge 2576 px, max 4784 visual tokens.
  - Standard tier, "All other models": 1568 px / 1568 tokens.
  - High-resolution support is automatic.
  - Source: [Anthropic Vision guide](https://platform.claude.com/docs/en/build-with-claude/vision)
- Worked examples:
  - 1920×1080 is downsized to 1456×819 (1560 tokens) on the standard tier. The high-res tier does not resize it (2691 tokens).
  - 3840×2160 becomes 2576×1449 (4784 tokens) on the high-res tier.
  - 1000×1000 is not resized on either tier (1296 tokens).
  - Source: [Anthropic Vision guide](https://platform.claude.com/docs/en/build-with-claude/vision)
- High-res images can use "roughly three times more visual tokens". Anthropic suggests downsampling when that fidelity is not needed — [Anthropic Vision guide](https://platform.claude.com/docs/en/build-with-claude/vision)
- Resizing details:
  - Claude resizes to the largest aspect-preserving size that fits both the edge limit and the token limit.
  - It then pads the bottom and right edges up to a multiple of 28 px.
  - "For nearly all photos and screenshots, the visual token limit is what determines the final size."
  - Source: [Anthropic Coordinates guide](https://platform.claude.com/docs/en/build-with-claude/vision-coordinates)
- An image block can set `"transformations": {"oversized_image": "error"}`. The request is then rejected instead of silently downscaled — [Anthropic Coordinates guide](https://platform.claude.com/docs/en/build-with-claude/vision-coordinates)

**Request limits**
- Images per request: 20 per message on claude.ai; 100 per API request for 200k-context models; 600 for all others. Max 8000×8000 px per image — [Anthropic Vision guide](https://platform.claude.com/docs/en/build-with-claude/vision)
- Above 20 images in one request, a stricter per-image dimension limit applies. Images from earlier turns and images inside `tool_result` count toward it. To stay safe, keep each side ≤2000 px or send ≤20 images — [Anthropic Vision guide](https://platform.claude.com/docs/en/build-with-claude/vision)
- Size and format: 10 MB per image (base64) on the API, 5 MB on Bedrock/Vertex, 32 MB per standard request. Formats are JPEG/PNG/GIF/WebP; for GIFs only the first frame is used. Claude "does not parse or receive any metadata from images" — [Anthropic Vision guide](https://platform.claude.com/docs/en/build-with-claude/vision)

**Documented limitations (verbatim categories)**
- "Claude might hallucinate or make mistakes when interpreting low-quality, rotated, or very small images under 200 pixels." — [Anthropic Vision guide](https://platform.claude.com/docs/en/build-with-claude/vision)
- Spatial reasoning: "Claude's coordinate and localization outputs are approximate… verify outputs before relying on them." — [Anthropic Vision guide](https://platform.claude.com/docs/en/build-with-claude/vision)
- Counting: "approximate counts… might not always be precisely accurate, especially with large numbers of small objects." — [Anthropic Vision guide](https://platform.claude.com/docs/en/build-with-claude/vision)
- AI-generated images: "Claude cannot determine whether an image is AI-generated… Do not rely on it to detect fake or synthetic images." — [Anthropic Vision guide](https://platform.claude.com/docs/en/build-with-claude/vision)
- "Do not use Claude for tasks requiring perfect precision or sensitive image analysis without human oversight." — [Anthropic Vision guide](https://platform.claude.com/docs/en/build-with-claude/vision)
- Image quality guidance:
  - Lossy compression "can introduce artifacts that are detrimental to model performance, especially when multiple compression passes are applied."
  - Inspect the images actually sent.
  - Pre-resize or crop rather than let the server shrink them.
  - Source: [Anthropic Vision guide](https://platform.claude.com/docs/en/build-with-claude/vision)

**Recommended prompting for image tasks**
- Order: "Claude works best when images come before text… prefer an image-then-text structure." — [Anthropic Vision guide](https://platform.claude.com/docs/en/build-with-claude/vision)
- Multiple images: Claude analyzes multiple images jointly, which suits "comparing images, asking about differences". Introduce each one "with a short text label (`Image 1:`, `Image 2:`…)". Later turns can refer back to earlier images without resending them — [Anthropic Vision guide](https://platform.claude.com/docs/en/build-with-claude/vision)
- Coordinates:
  - "Claude works best with absolute pixel coordinates. Ask for them explicitly… Claude does not work well when you ask for normalized coordinates, for example… between 0 and 1000."
  - "Small elements lose precision when an image is downscaled: for fine targets, crop the region of interest and send the crop (offset returned coordinates by the crop origin), or use a high-resolution-tier model."
  - Source: [Anthropic Coordinates guide](https://platform.claude.com/docs/en/build-with-claude/vision-coordinates)
- Opus 4.5/4.6 have "improved vision capabilities… particularly when there are multiple images present in context." On the crop tool: "Testing has shown consistent uplift on image evaluations when Claude is able to 'zoom' in on relevant regions of an image." — [Claude prompting best practices](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/claude-prompting-best-practices)

**Opus 5.5 (current model)**
- It reads charts, diagrams and screenshots more accurately than Opus 5. Even at its lowest effort it read dense charts better than Opus 5 at its highest.
- It is better "where meaning depends on position rather than text: which boxes an arrow connects… what changed between two versions of a diagram."
- Source: [Prompting Claude Opus 5.5](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-opus-5-5)
- Anthropic's instructions for the densest inputs on Opus 5.5:
  - Re-test whether older visual scaffolding is still needed.
  - "Higher-resolution images help, most of all for inputs like technical drawings."
  - "image-processing tools: run the model as an agent with access to a container that holds the raw images and has libraries such as PIL and OpenCV installed, so that it can crop, zoom, measure, and verify its work. If a container is too much overhead, a cropping tool alone still helps."
  - "The model uses these tools more effectively at higher effort levels. Without tools, raising effort improves its reading of technical drawings but does little for charts."
  - Source: [Prompting Claude Opus 5.5](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-opus-5-5)
- Prompts that ask the model to write its reasoning into the response "may be declined with the `reasoning_extraction` refusal category." A short explanation of the answer is still fine — [Prompting Claude Opus 5.5](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-opus-5-5)

**Crop/zoom tool recipe (Anthropic cookbook)**
- The tool takes integer absolute-pixel `x1,y1,x2,y2` plus an optional `image_index`.
- The crop is cut from the full-resolution original, not the downscaled copy, then upscaled to the image budget and returned as JPEG q92.
- Rationale: detail smaller than a 28 px patch is lost and "no amount of prompting recovers it"; zoom is "the lean-in move."
- Source: [Anthropic crop tool cookbook](https://platform.claude.com/cookbook/multimodal-crop-tool)
- Measured uplift on Chartography (100 chart questions, model-graded):

  | Model | Without tool | With zoom | Mean cost/question |
  |---|---|---|---|
  | claude-fable-5 | 29% | 73% | $0.08 → $0.95 |
  | claude-sonnet-5 | 13% | 44% | — |

  - On a near-tie question, fable-5 was wrong in 36/40 runs without the tool and right in 40/40 with it.
  - Smaller models benefit less.
  - Source: [Anthropic crop tool cookbook](https://platform.claude.com/cookbook/multimodal-crop-tool)

**Independent measurements of Claude vision (older generations)**
- "VLMs are blind" (BlindTest: overlapping circles, line intersections, etc.):
  - 4 SOTA VLMs averaged 58.07%.
  - Sonnet-3.5 was best at 77.84%, against an expected human 100%. Earlier paper versions report different numbers.
  - Simplifying images raised Sonnet-3's overlapping-circle counting by +63 points.
  - Linear probes show the vision encoder already holds the information (≥99.47%), so the failure lies in turning it into language.
  - Source: [Rahmanzadehgervi et al., ACCV 2024 / arXiv v4](https://arxiv.org/html/2407.06581v4)
- VideoGameQA-Bench (2025) visual regression testing (reference vs modified game frame):

  | Model | Accuracy |
  |---|---|
  | o4-mini | 45.2% |
  | o3 | 39.6% |
  | GPT-4o | 31.6% |
  | Gemini-2.5-Pro | 30.8% |
  | Sonnet-3.7 | 24.0% |
  | Sonnet-3.5 | 14.0% |

  Source: [Taesiri et al., VideoGameQA-Bench](https://arxiv.org/html/2505.15952v2)

**Other vendors, for contrast**
- Gemini:
  - Bounding boxes are normalized 0–1000 as `[ymin, xmin, ymax, xmax]`.
  - Images ≤384 px cost 258 tokens; larger images are tiled into 768×768 tiles at 258 tokens each.
  - A `media_resolution` parameter caps tokens per image.
  - Gemini's docs say: "When using a single image with text, place the text prompt before the image." This is the opposite of Anthropic's image-first advice.
  - Source: [Gemini image understanding docs](https://ai.google.dev/gemini-api/docs/image-understanding)
- OpenAI o3/o4-mini (April 2025) "think with images": they crop, zoom and rotate uploaded images inside their chain of thought natively — [OpenAI, Thinking with images](https://openai.com/index/thinking-with-images/)

### Inferences
- For texture and asset review, resolution budget is the first constraint.
  - On Opus 5.5 (high-res tier), a side-by-side of two 1024² textures (2048×1024) fits without downscaling: ⌈2048/28⌉×⌈1024/28⌉ = 74×37 = 2738 tokens < 4784.
  - A 2×2 grid of 1024² images (2048×2048) is 74×74 = 5476 tokens. That exceeds the budget and will be downscaled.
  - Compute the budget before composing grids, and use `oversized_image: "error"` to catch silent shrinking.
- Prompt wording cannot fix fine-detail judgements that fall below patch size: tiling seams, normal-map strength, 1–4 px artifacts, alpha fringes. Use crops at native resolution or code measurement instead.
- Placement conventions differ by vendor (Claude: image first; Gemini: text first). A multi-vendor judging harness should follow each vendor's guidance rather than share one template.
- Given the `reasoning_extraction` refusal note, Opus 5.5 critique prompts should ask for structured evidence per finding (region, measurement, verdict), not "show your full reasoning."

### Gaps
- I found no Anthropic-published numbers for Claude-specific counting or spatial accuracy on the current generation, beyond the Chartography zoom result and qualitative claims for Opus 5.5. I did not check the system cards.
- Anthropic says nothing about aesthetic or art-direction judgement (style consistency, colour harmony). The only vision guidance is about reading and locating content.
- No public benchmark of Opus 4.7+/5.x on BlindTest, BLINK or VideoGameQA-Bench turned up, so the old Sonnet-3.x numbers may be stale.

## Q2. What do VLM-as-a-judge and image-evaluation papers (2024–2026) find about reliability, positional bias, leniency/sycophancy, and what improves agreement with humans?

### Takeaway
VLM judges agree with humans much better on pairwise comparison than on absolute scores or rankings. They are lenient: scores cluster high, and absolute pass rates run far above human ones. Superficial changes to an image sway them: boxes, overlays and brightness changes inflate scores even under bias-aware prompts. They show position bias in multi-image input; GPT-4o-class models are weakest on middle images, and small open models over-pick the first slot. Their main weakness is basic perception, not reasoning. What helped: atomic yes/no questions with dependencies (DSG), pairwise instead of absolute scoring, natural-language Likert labels instead of numbers, multiple order permutations with consistency checks, ensembling, and specialist detectors or code for countable properties. Free-form CoT did not reliably help in GPT-4V-era studies.

### Cited Findings
**MLLM-as-a-Judge (ICML 2024; GPT-4V, Gemini, LLaVA, Qwen-VL)**
- Agreement with humans:

  | Model | Scoring (Pearson) | Pair, with tie | Pair, no tie | Batch ranking (distance, lower = better) |
  |---|---|---|---|---|
  | GPT-4V | 0.490 | 0.636 | 0.773 | 0.361 |
  | Gemini | 0.304 | 0.509 | 0.615 | 0.432 |

  The paper concludes MLLMs are not yet "fully reliable evaluators." — [Chen et al., arXiv 2402.04788](https://arxiv.org/html/2402.04788v3)
- Biases:
  - High-score bias: Gemini, LLaVA and CogVLM mostly give scores around 4. Qwen-VL gives 80% of its scores in the 4–5 range.
  - Length bias: longer answers with the same meaning raised GPT-4V's scores by about 0.6 and Gemini's by about 0.75.
  - Egocentric bias: GPT-4V shows slight self-preference.
  - Position/pattern bias: LLaVA copied the prompt's "ABCD" order in 88.2% of batch rankings.
  - Source: [Chen et al.](https://arxiv.org/html/2402.04788v3)
- Consistency over 6 repeated runs: the majority-consistency ratio was 0.611 for GPT-4V and 0.054 for Gemini on scoring, and 0.675 for GPT-4V on pairs — [Chen et al.](https://arxiv.org/html/2402.04788v3)
- Chain-of-thought (3-step) lowered agreement:
  - GPT-4V scoring: 0.557 → 0.299. GPT-4V pairs: 0.806 → 0.728.
  - It did reduce hallucinations.
  - Caveat: the Table 5 baselines differ from the main Table 2 averages, so these are subset numbers.
  - Source: [Chen et al.](https://arxiv.org/html/2402.04788v3)

**MJ-Bench (NeurIPS 2025 D&B; judges for text-to-image feedback)**
- Closed VLMs did best overall, with GPT-4o highest on average. Claude 3 Opus was also tested.
- Small CLIP-style scorers did better on text-image alignment and image quality; VLMs did better on safety and bias.
- "Natural-language (Likert-style) scales gave more accurate and stable feedback than numerical scales."
- Sources: [MJ-Bench arXiv 2407.04842](https://arxiv.org/abs/2407.04842); [NeurIPS 2025 page](https://proceedings.neurips.cc/paper_files/paper/2025/hash/59d2eaa5842fa641ff9b8e4c7ff0f6ee-Abstract-Datasets_and_Benchmarks_Track.html)

**VL-RewardBench (CVPR 2025)**
- GPT-4o reached only 65.4% accuracy.
- Perception errors dominate over reasoning errors. Examples: GPT-4o-mini had a 67.9% error rate on Existence tasks, and Qwen2-VL-7B failed 80.9% of Recognition tasks. Reasoning tasks averaged a 41.8% error rate.
- Source: [Li et al., arXiv 2411.17451](https://arxiv.org/abs/2411.17451)

**Fooling the LVLM judges (2025; GPT-4.1, GPT-4o, o3, LLaVA, Qwen2.5-VL)**
- Eight visual manipulations inflate scores: bounding boxes, "Reference Image" text, keyword overlay, instruction overlay, beauty filter, brightness, gamma, black padding. Examples:
  - GPT-4o, Indoor, bounding box: 1.14 → 2.05 (+80.2%).
  - GPT-4o, Animal, bounding box: +77.9%.
  - GPT-4.1, Animal, bounding box: +18.0%.
  - Instruction overlay inflated scores across all judges.
- Pairwise tests with order swapped and averaged also flipped rankings.
- Bias-aware prompts, bias-definition prompts and CoT gave only "partial improvements". CoT raised baseline scores (1.36 → 1.72) and still rewarded bounding boxes.
- Source: [arXiv 2505.15249](https://arxiv.org/html/2505.15249)

**Position bias in multi-image input**
- CVPR 2025: open-source models do best on images placed later. Proprietary models such as GPT-4o do best on the first and last images and struggle in the middle. The training-free SoFA attention fix reduces this — [Tian et al., CVPR 2025](https://arxiv.org/html/2503.13792v1)
- "When the Judge Acts" (October 2026; Qwen3-VL-4B/8B, SmolVLM2):
  - The 4B judge picked slot A in 48.8% of 900 calls against 27.8% chance. Its choice changed on 59.7% of prompts when the order changed. At 8B, the first-slot share was 27.3%, but choices still changed on 43.0%.
  - Majority over 3 cyclic orders raised gain-over-random from 0.039 to 0.065, at about 2.6× the cost.
  - Unanimity across orders worked as an abstention gate for 4B. For 8B it "mostly discards good decisions", so the gate "must be re-audited when the judge changes."
  - No frontier models were tested.
  - Source: [arXiv 2610.01243](https://arxiv.org/html/2610.01243)

**Judges that under-use the image**
- "Informativeness bias" (2026): VLM judges "pay limited attention to the image" and prefer the more informative answer even when it contradicts the image. The BIRCH method first corrects candidate answers against the image, then compares them; it reduces the bias by up to 17% — [arXiv 2604.17768](https://arxiv.org/abs/2604.17768)
- An ICML 2026 talk (KAIST/KRAFTON, slides only, not peer-reviewed) targets judges failing to penalize visual claims that contradict the image — [ICML 2026 slides](https://icml.cc/media/icml-2026/Slides/61108.pdf)

**Decomposition into atomic yes/no questions**
- TIFA (ICCV 2023):
  - An LLM writes question–answer pairs from the prompt; a VQA model answers them on the image.
  - The benchmark has 4K prompts, 25K questions and 12 categories.
  - It correlates better with humans than earlier metrics.
  - T2I models were weak at counting, spatial relations and multi-object composition.
  - Source: [Hu et al., ICCV 2023](https://openaccess.thecvf.com/content/ICCV2023/html/Hu_TIFA_Accurate_and_Interpretable_Text-to-Image_Faithfulness_Evaluation_with_Question_Answering_ICCV_2023_paper.html)
- DSG / Davidsonian Scene Graph (ICLR 2024). Questions are atomic tuples (entity, attribute, relation) with parent–child dependencies, and a child question is skipped when its parent fails.
  - Atomicity: DSG 96.5% vs TIFA 78.5%.
  - Using dependencies raised PaLI's per-question match with humans from 65.2% to 73.8%.
  - Spearman with human Likert: DSG 0.571, TIFA 0.431, CLIPScore 0.276. The paper's text gives 0.563 in one place.
  - Categories where VQA failed:

    | Category | PaLI match with humans |
    |---|---|
    | Text rendering | 13.7% |
    | Style | 21.1% |
    | Shape | 53.9% |
    | Material | 59.1% |
    | Color | 83.6% |

    Counts up to 5 were wrong about 40% of the time.
  - Leniency: the VQA judge's absolute scores (~80–84%) ran well above human ones (~59–66%), even though the model ranking matched.
  - Source: [Cho et al., arXiv 2310.18235](https://arxiv.org/html/2310.18235v4)
- VQAScore (2024):
  - Score = P("Yes") for "Does this figure show '{text}'?".
  - It beats CLIPScore. The CLIP-FlanT5 version "outperforms even the strongest baselines that make use of the proprietary GPT-4V."
  - GenAI-Bench has 1,600 designer-written compositional prompts and 15,000+ human ratings.
  - Ranking 3–9 candidates by VQAScore improves human ratings more than PickScore, HPSv2 or ImageReward.
  - Sources: [Lin et al., arXiv 2404.01291](https://arxiv.org/abs/2404.01291); [GenAI-Bench arXiv 2406.13743](https://arxiv.org/abs/2406.13743)
- GenEval (NeurIPS 2023 D&B) measures instead of asking:
  - An object detector with boxes and masks checks object presence, counts and relative position.
  - A separate classifier checks colour.
  - Secondary sources report ~83% agreement with humans; I did not verify this in the paper.
  - Source: [Ghosh et al., arXiv 2310.11513](https://arxiv.org/abs/2310.11513)

**Multi-view pairwise judging for 3D assets**
- GPTEval3D (CVPR 2024):
  - GPT-4V receives multi-view renders of two 3D assets and compares them on user-defined criteria.
  - The pairwise results feed an Elo rating, which "strongly aligns with human preference across different evaluation criteria."
  - Hallucinations and systematic errors remain; a secondary summary says ensembling mitigates them.
  - Sources: [Wu et al., CVPR 2024](https://openaccess.thecvf.com/content/CVPR2024/html/Wu_GPT-4Vision_is_a_Human-Aligned_Evaluator_for_Text-to-3D_Generation_CVPR_2024_paper.html); [arXiv 2401.04092](https://arxiv.org/abs/2401.04092)

**Perception ceilings and human-preference data**
- BLINK (ECCV 2024): 14 classic perception tasks, including relative depth, correspondence and multi-view reasoning. GPT-4V scored 51.26% and Gemini ~45%, against 95.7% for humans — only about 13 and 8 points above chance — [Fu et al., arXiv 2404.12390](https://arxiv.org/abs/2404.12390)
- VisionArena (CVPR 2025; 230K real conversations, 45 VLMs): style strongly drives human preference on open-ended tasks. VLMs "struggle with spatial reasoning and planning." — [Chiang/Dunlap et al., arXiv 2412.08687](https://arxiv.org/abs/2412.08687)

**Sycophancy**
- MM-SY (ICLR 2025) measures VLMs abandoning visual evidence under user pushback across 10 tasks. The effect varies by task, tone and model size — [ICLR 2025 paper](https://proceedings.iclr.cc/paper_files/paper/2025/file/07be1a0850e58ca29e2b6ce31fc0c791-Paper-Conference.pdf)
- PENDULUM (December 2025; about 2,000 VQA pairs): a model correctly counted 3 players, then switched to the user's suggested "2" — [arXiv 2512.19350](https://arxiv.org/pdf/2512.19350v1). Search-index attribution; I did not verify it in the full text.

**Thinking with drawings and tools**
- Visual Sketchpad (NeurIPS 2024): GPT-4o writes code to draw auxiliary lines and boxes, or calls detectors and segmenters, on the image. This gave +8.6% on vision tasks, 80.3% on V*Bench and 83.9% on BLINK spatial reasoning — [Hu, Shi et al., arXiv 2406.09403](https://arxiv.org/abs/2406.09403)

### Inferences
- For asset acceptance, prefer pairwise "new vs reference" verdicts to absolute 1–10 scores. Every study that compared formats found pairwise closest to humans, and absolute scores run high.
- Treat "PASS" from a VLM judge as optimistic by default: high-score bias, leniency in DSG absolute scores, sycophancy. Require evidence per criterion and keep human spot-checks.
- Presentation artefacts can bias the judge: debug overlays, bounding boxes, captions burned into the image, different brightness or exposure between captures. Capture original and new under identical camera, light and exposure. Keep annotation overlays out of the image being judged; put annotations in a separate image or in the text.
- Position bias is real but model-specific. Swapping order and accepting only consistent verdicts is cheap insurance for 2 images. With more than 2 images, rotate orders and avoid relying on middle positions.
- Free-form CoT was not a reliable fix in GPT-4V-era judges. Opus 5.5 always thinks internally, so the lever is structure (atomic questions, per-criterion fields, measurements), not "think step by step."

### Gaps
- No 2025–2026 controlled study turned up that isolates the effect of a rubric or checklist versus a holistic prompt on human agreement for image judging, and none for Claude specifically. A search hit claimed Gemini-3-Flash reached ρ=0.87 with a checklist plus rubric, but I could not attribute it to a specific paper, so it is omitted.
- I found no position-bias measurement for Claude models in image pairwise judging.
- MJ-Bench's per-model numbers for Claude 3 Opus on alignment and quality were not extracted. Only preference-bias metrics appeared in snippets.

## Q3. Which practical techniques give strict visual QA from an LLM, and what concrete prompt patterns follow?

### Takeaway
The techniques that hold up all turn a vague "does this look good?" into checks that can be verified:
- atomic, spec-derived yes/no questions (TIFA/DSG)
- pairwise comparison against a reference, in both orders (MLLM-as-a-Judge, GPTEval3D, position-bias work)
- per-criterion verdicts with word labels (MJ-Bench)
- zooming into crops at native resolution (Anthropic's crop tool)
- measuring with code instead of eyeballing (Anthropic's PIL/OpenCV container advice, GenEval, Visual Sketchpad)

Practitioners use VLMs as a complement to pixel diffs, not a replacement: VLMs suppress noise but miss real changes.

### Cited Findings
- Pixel diff vs VLM on UI change detection (WUICC-bench, 2026 preprint):

  | Method | No-change accuracy | Change accuracy |
  |---|---|---|
  | Pixel diff | 0% (flags everything) | 100% |
  | Qwen2-VL-7B | 99.63% | 65.68% (misses real changes) |
  | Llama-3.2-11B | 21.64% | 79.39% |
  | Trained difference-captioning models | 85.8–97.4% | 93.6–96.2% |

  - Frontier closed VLMs were not tested.
  - Source: [WUICC-bench, arXiv 2607.01728](https://arxiv.org/html/2607.01728)
- In game visual regression testing, models miss changes that are subtle (an object in the background) as well as obvious ones (close to the camera). Reasoning variants beat non-reasoning ones within a family (o3 > GPT-4o) — [VideoGameQA-Bench](https://arxiv.org/html/2505.15952v2)
- Pixel-diff noise and AI-diff tooling, per practitioner and vendor pages:
  - Pixel diffs flag anti-aliasing and margin noise, and teams that cannot tune thresholds end up ignoring the output.
  - Applitools-style "AI diff" is computer vision, not an LLM.
  - LLM "smart visual testing" pages are vendor marketing.
  - A practitioner guide advises VLM assertions "as a complement to pixel diffs, not a replacement."
  - Sources: [Applitools](https://applitools.com/blog/visual-regression-testing/); [TestMu blog](https://www.testmuai.com/blog/smart-visual-testing-with-llms/); [drizz.dev](https://dev.to/drizzdev/mobile-visual-regression-testing-in-2026-why-vision-ai-catches-what-script-based-tools-miss-2bfm). These are search-snippet level and vendor sources.
- Zoom and crop measurably fix sub-patch detail: Chartography +44 points for fable-5 — [Anthropic crop tool cookbook](https://platform.claude.com/cookbook/multimodal-crop-tool)
- Anthropic recommends a container with PIL/OpenCV "so that it can crop, zoom, measure, and verify its work." — [Prompting Claude Opus 5.5](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-opus-5-5)
- Measurement beats asking for countable and positional properties: GenEval uses a detector and colour classifier for count, position and colour — [GenEval](https://arxiv.org/abs/2310.11513)
- Drawing auxiliary lines or boxes with code improves VLM spatial reasoning — [Visual Sketchpad](https://arxiv.org/abs/2406.09403)
- Simplifying images (removing clutter) sharply improves counting, e.g. +63 points for Sonnet-3 on overlapping circles — [VLMs are blind](https://arxiv.org/html/2407.06581v4)
- Atomic questions with dependency gating improve agreement and reduce contradictory answers — [DSG](https://arxiv.org/html/2310.18235v4)
- Order swapping and averaging is the standard control. Majority over cyclic orders helps, and order-consistency can gate abstention, but only after auditing for the specific judge:
  - [Fooling LVLM judges](https://arxiv.org/html/2505.15249)
  - [When the Judge Acts](https://arxiv.org/html/2610.01243)
  - [Position bias in LLM-as-a-judge, IJCNLP 2025](https://aclanthology.org/2025.ijcnlp-long.18.pdf)
- Word labels beat numeric scales for stability — [MJ-Bench](https://arxiv.org/abs/2407.04842)
- Anchoring in a verified description before comparing reduces "judging without looking" — [BIRCH / informativeness bias](https://arxiv.org/abs/2604.17768)
- Claude-specific mechanics:
  - Label images "Image 1:", "Image 2:" and put images before the question — [Anthropic Vision guide](https://platform.claude.com/docs/en/build-with-claude/vision)
  - Ask for pixel `[x1,y1,x2,y2]` coordinates and use structured outputs for JSON — [Anthropic Coordinates guide](https://platform.claude.com/docs/en/build-with-claude/vision-coordinates)
- Frontend-design guidance for Opus 5.5: "avoid a generic AI look" mostly swaps one default for another. Naming specific patterns works better. By analogy, critique criteria should name concrete failure patterns rather than ask generally whether something "looks good" — [Prompting Claude Opus 5.5](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-opus-5-5)

### Inferences
The following is a synthesized protocol and set of prompt patterns, derived from the findings above. It is not itself a published method.

**Protocol for strict review (original vs new asset)**
1. **Write the spec before looking.** Turn the art direction into atomic, checkable yes/no criteria, DSG-style, before showing any image. Gate dependent questions: if "is it a wooden door?" fails, skip "are the planks vertical?". Name concrete failure patterns, such as "visible tiling repeat at 2+ tiles", "normal relief reads stronger than the reference", "alpha fringe halo", "brightness differs from replaced texture". Avoid "looks good" criteria.
2. **Measure what code can measure, and give Claude the numbers.**
   - Brightness and contrast: mean and percentile luminance, histogram overlap against the replaced texture.
   - Hue/saturation shift.
   - Tiling seams: offset the texture by 50% and inspect the seams; autocorrelation peaks.
   - Normal-map strength: mean |∇| or normal-z distribution against the reference.
   - Size and proportion: silhouette bounding boxes and ratios in px.
   - Alpha coverage.
   - SSIM or ΔE heatmaps against the reference.

   The VLM then judges meaning, while code holds the facts. This follows Anthropic's PIL/OpenCV advice, GenEval and the counting limits.
3. **Use same-scale side-by-sides.**
   - Same camera, light, exposure and crop; same pixel scale; labelled `Image 1`/`Image 2` as separate image blocks, or one composite with neutral labels (A/B, not "old/new").
   - Check the token budget so neither image is downscaled differently (Opus 5.5: ≤4784 tokens and ≤2576 px per image).
   - Keep debug overlays and boxes out of the images being judged.
4. **Look closer.** Add native-resolution crops of risky regions (seams, edges, faces, text) or provide the zoom tool. Do not expect detail below 28 px to survive a downscaled full view.
5. **Find defects first, then score per criterion.** Ask for an exhaustive defect list with pixel regions before any overall verdict. Then give a per-criterion verdict with a word scale (fail / weak / acceptable / strong) and one evidence line each. Overall acceptance is a rule over the criteria (e.g. any "fail" means reject), not the model's gestalt.
6. **Compare in both orders.** Run the A/B comparison twice with the order swapped. Accept only consistent verdicts; an inconsistent one counts as "no clear winner" and goes to human review. For more than 2 candidates, rotate orders and avoid middle-position reliance.
7. **Hide provenance and stay firm.** Do not say which image is "ours" or "the new one", and do not state the hoped-for outcome. Do not argue a verdict into a pass: re-run fresh rather than pushing back in the same thread, because of sycophancy.
8. **Review multiple views of 3D.** Use a fixed turntable set (front, 3/4, side, back, top, close-up at gameplay distance) for both reference and candidate, judged pairwise per view, plus an in-game shot at real viewing distance.
9. **Calibrate.** Keep a small set of human-labelled accept/reject pairs and re-check the judge's agreement whenever the model or prompt changes.

**Prompt pattern A — spec-first atomic checklist (single image)**
```text
[Image 1: candidate texture, 1024x1024, native resolution]
[Image 2: crop 1 at 4x — top-left seam region]
Spec (from art bible): <list>.
Measurements computed by code: mean L* = 41.2 (reference 47.8), tile-offset seam ΔE max = 9.3, normal |∇| mean = 0.18 (reference 0.05).
Answer each check independently. For each: verdict yes/no/cannot-tell, the pixel region [x1,y1,x2,y2] in Image 1 that supports it, one line of evidence. Do not answer a child check if its parent is "no".
1. Is the material recognisably <X>? 
1a. Are the planks vertical? (only if 1 = yes)
2. Is any repeat pattern visible when tiled 2x2? ...
Then: list every defect not covered above. Then: overall = REJECT if any check is "no".
```

**Prompt pattern B — reference vs candidate pairwise, order-swapped**
```text
Image 1: <A>   Image 2: <B>   (same camera, light, exposure, pixel scale)
Reference intent: <one paragraph: object, role, period, material, how it must read at gameplay distance>.
Criteria (judge each separately; word scale: worse / same / better / cannot-tell):
silhouette & proportion, material read, value/brightness vs surroundings, relief strength, tiling/repetition, colour palette fit, artefacts.
First list concrete differences you can point to, with pixel regions in each image. Then the per-criterion verdicts. Do not give an overall preference unless at least one criterion differs.
```
Run it again with A and B swapped and only keep criteria with matching verdicts.

**Prompt pattern C — defects-first audit (counters leniency)**
```text
You are doing acceptance QA, not giving encouragement. Assume there are defects.
List up to 15 defects, most severe first: region [x1,y1,x2,y2], category (seam, stretch, blur, halo, scale, lighting mismatch, style break, wrong material), severity (blocker/major/minor), and what you would measure to confirm it.
If you find none in a category, say "none found" for that category.
```

**Prompt pattern D — measure, don't eyeball (Claude with code tools)**
```text
Before judging, use Python (PIL/OpenCV/numpy) on the raw files to compute: luminance histogram and mean vs the reference, seam ΔE after a 50% offset, normal-map gradient magnitude, alpha edge width, bbox aspect ratio. Save diff/heatmap images and crops of the 3 worst regions and look at them. Report the numbers, then the visual verdict, and flag any disagreement between them.
```

### Gaps
- I found no published, controlled evaluation of the defects-first and per-criterion patterns specifically for image critique. They are reasoned from leniency, high-score bias and sycophancy findings, not tested directly.
- I found no practitioner write-up with measured false-positive or false-negative rates for Claude or GPT used as a visual-regression judge on real UI or game screenshots, beyond preprint benchmarks of small open models (WUICC) and VideoGameQA-Bench.
- I found no evidence on whether one composite side-by-side image beats two separate image blocks for comparison accuracy in Claude.

## Q4. Have these techniques been used in game art, 3D or texture review pipelines?

### Takeaway
I found published work on VLMs for game QA (glitch detection, visual regression against a reference frame) and on VLM judging of 3D assets from multi-view renders (GPTEval3D). I found no published pipeline that uses VLMs to review game textures or art-direction fidelity. Results in this area show that reference-based comparison is the hardest task, that false positives (hallucinated clipping) and model-specific blanket biases ("everything is a glitch" or "everything is clean") are common, and that full autonomy is "not yet feasible."

### Cited Findings
- VideoGameQA-Bench (2025; GPT-4.x, o3/o4-mini, Gemini 2.x, Sonnet 3.5/3.7, open models):

  | Task | Best results |
  |---|---|
  | Image glitch detection | GPT-4o 82.8% |
  | Parametric clipping | GPT-4.1 87.8% (misses borderline overlaps) |
  | Visual unit tests | Gemini-2.5-Pro 53.0% |
  | Visual regression | o4-mini 45.2% best; Sonnet-3.7 24.0% |
  | Bug report generation | GPT-4o ~54% |

  - Visual unit tests check specific details such as number of visible eyes, hand position, inside or outside a room, a car door open or closed. Counting and facing direction were inconsistent.
  - Failure modes: unusual body configurations, intricate clipping, semantic glitches, hallucinated clipping. Gemma-3 labels nearly everything glitchy; Llama-4 labels nearly everything clean.
  - The authors conclude fully autonomous glitch detection "might not yet be feasible for real-world use" because of false positives.
  - Source: [Taesiri et al.](https://arxiv.org/html/2505.15952v2)
- The VideoGameQA-Bench regression set was built from Unity Asset Store scenes with objects removed, each paired with an unaltered reference capture — [VideoGameQA-Bench PDF](https://asgaard.ece.ualberta.ca/papers/Conference/ARXIV2025_Taesiri_VideoGameQA-Bench.pdf)
- RefGlitch-Bench (2026):
  - The judge compares the test frame with a reference frame from the same video, to separate "true glitches from benign visual variation."
  - Automatic reference selection ("LastCleanFrame" was best) "transfers across VLMs."
  - The named challenges are reference reliability and error propagation.
  - Source: [arXiv 2604.11082](https://arxiv.org/abs/2604.11082)
- A screenshot can show a missing object, but diagnosing the cause may need game state before and after an input — [AI for Games in the Foundation Model Era, arXiv 2609.16679](https://arxiv.org/pdf/2609.16679)
- 3D asset judging: GPTEval3D uses multi-view renders, pairwise GPT-4V comparisons on user-defined criteria and Elo aggregation — [Wu et al., CVPR 2024](https://openaccess.thecvf.com/content/CVPR2024/html/Wu_GPT-4Vision_is_a_Human-Aligned_Evaluator_for_Text-to-3D_Generation_CVPR_2024_paper.html). The exact criterion list was not read in fetched text.
- Industry guide (2026, blog): AI-generated static props still need UV and PBR checks, and hero assets need texture repainting by humans — [wavect.io](https://wavect.io/blog/ai-3d-model-generators-game-development-2026/)

### Inferences
- The closest validated designs for a texture/asset acceptance loop:
  - reference-based pairwise comparison (RefGlitch, VideoGameQA regression)
  - multi-view pairwise judging with aggregation (GPTEval3D)
  - spec-derived atomic checks (DSG)

  Combining them gives one protocol: replaced original vs new, the same views, per-criterion pairwise verdicts in both orders, code-measured brightness, relief and tiling, and in-game captures at gameplay distance.
- Reference-based regression was the weakest VLM task in 2025 (Sonnet-3.7 at 24%), so "spot what changed" should be done by code first (diff heatmaps, ΔE, SSIM). The VLM's job is then to say whether a measured change matters, which matches WUICC's finding that VLMs are good at suppressing noise but miss changes.
- Blanket-bias failure modes (always "glitch" or always "clean") argue for keeping known-good and known-bad control pairs in every batch, to detect a judge that is passing everything.

### Gaps
- I found no published pipeline or study on VLM review of game textures specifically: PBR maps, tiling, normal strength, art-direction consistency with an existing game's style. This is an open area.
- "Human-AI Collaborative Game Testing with Vision Language Models" ([arXiv 2501.11782](https://arxiv.org/pdf/2501.11782)) appeared in search, but I did not fetch it, so its findings are not reported.
- I found no data on Claude 4.7+/5.x performance on VideoGameQA-Bench or RefGlitch-Bench. The Claude numbers above are from Sonnet 3.5/3.7 and are likely stale given Anthropic's stated vision gains in Opus 4.5 → 5.5.

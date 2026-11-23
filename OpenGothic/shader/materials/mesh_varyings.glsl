// Included inside a mesh stage: stores the varyings `var` of the vertex `laneID`.
    /*
        Workaround the AMD driver issue where assigning to a vec3 output results in a driver crash.
        Here we unroll the shOut[laneID] = var assignment into individual fields and apply the workaround.
        Once AMD fixes the issue the original code can be restored.
        Original code: 
            #if defined(MAT_VARYINGS)
                shOut[laneID] = var;
            #endif
        Link to the discussion on AMD forums: https://community.amd.com/t5/newcomers-start-here/driver-crash-when-using-vk-ext-mesh-shader/m-p/667490
        Link to the github issue: https://github.com/Try/OpenGothic/issues/577
    */
#if defined(MAT_COLOR)
    shOut[laneID].color = var.color;
#endif
#if defined(MAT_UV)
    shOut[laneID].uv = var.uv;
#endif
#if defined(MAT_POSITION)
    shOut[laneID].pos.xyz = var.pos; // pos is vec3, applying the workaround
#endif
#if defined(MAT_NORMAL)
    shOut[laneID].normal.xyz = var.normal; // normal is vec3, applying the workaround
#endif
#if defined(MAT_MOTION)
    shOut[laneID].prevStatic.xyz = var.prevStatic; // vec3, the same workaround
    shOut[laneID].prevDelta.xyz  = var.prevDelta;
#endif

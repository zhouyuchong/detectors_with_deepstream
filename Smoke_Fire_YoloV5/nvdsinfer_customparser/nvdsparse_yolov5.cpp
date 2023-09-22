/*
 * Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <algorithm>
#include <map>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <unordered_map>

#include "nvdsinfer_custom_impl.h"

static const int NUM_CLASSES_YOLO = 2;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLIP(a, min, max) (MAX(MIN(a, max), min))
#define NMS_THRESH 0.4
#define CONF_THRESH 0.1
#define BATCH_SIZE 1

extern "C" bool NvDsInferParseCustomYoloV5(
    std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo,
    NvDsInferParseDetectionParams const &detectionParams,
    std::vector<NvDsInferParseObjectInfo> &objectList);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static constexpr int LOCATIONS = 4;
struct alignas(float) Detection{
        //center_x center_y w h
        float bbox[LOCATIONS];
        float conf;  // bbox_conf * cls_conf
        float class_id;
    };

float iou(float lbox[4], float rbox[4]) {
    float interBox[] = {
        std::max(lbox[0] - lbox[2]/2.f , rbox[0] - rbox[2]/2.f), //left
        std::min(lbox[0] + lbox[2]/2.f , rbox[0] + rbox[2]/2.f), //right
        std::max(lbox[1] - lbox[3]/2.f , rbox[1] - rbox[3]/2.f), //top
        std::min(lbox[1] + lbox[3]/2.f , rbox[1] + rbox[3]/2.f), //bottom
    };

    if(interBox[2] > interBox[3] || interBox[0] > interBox[1])
        return 0.0f;

    float interBoxS =(interBox[1]-interBox[0])*(interBox[3]-interBox[2]);
    return interBoxS/(lbox[2]*lbox[3] + rbox[2]*rbox[3] -interBoxS);
}

bool cmp(Detection& a, Detection& b) {
    return a.conf > b.conf;
}


void decodeandnms(std::vector<Detection>& res, float *output, float conf_thresh, float nms_thresh){
    // int det_size = sizeof(Detection) / sizeof(float);
    int det_size = 7;
    float tmp_conf = 0;
    float tmp_fire_conf = 0;
    float tmp_smoke_conf = 0;
    int count = 0;
    std::map<float, std::vector<Detection>> m;
    for (int i = 0; i < 25200; i++) {
        if (output[det_size * i + 4] <= conf_thresh) continue;

        // count++;
        tmp_conf = output[det_size * i + 4];
        Detection det;
        det.conf = tmp_conf;
        det.bbox[0] = output[det_size * i]     - output[det_size * i + 2]/2.f;
        det.bbox[1] = output[det_size * i + 1] - output[det_size * i + 3]/2.f;
        det.bbox[2] = output[det_size * i]     + output[det_size * i + 2]/2.f;
        det.bbox[3] = output[det_size * i + 1] + output[det_size * i + 3]/2.f;
        
        tmp_fire_conf = tmp_conf * output[det_size * i + 5];
        tmp_smoke_conf = tmp_conf * output[det_size * i + 6];

        if(tmp_fire_conf > tmp_smoke_conf){
            det.class_id = 0;
        }
        else{
            det.class_id = 1;
        }
        // std::cout<<det.bbox[0]<<" "<<det.bbox[1]<<" "<<det.bbox[2]<<" "<<det.bbox[3]<<" "<<"  "<<det.conf<<"  "<<det.class_id<<std::endl;
        if (m.count(det.class_id) == 0) m.emplace(det.class_id, std::vector<Detection>());
        m[det.class_id].push_back(det);
    }
    // std::cout<<"get "<<count<<" after decode.."<<std::endl;
    count = 0;
    for (auto it = m.begin(); it != m.end(); it++) {
        auto& dets = it->second;
        std::sort(dets.begin(), dets.end(), cmp);
        for (size_t m = 0; m < dets.size(); ++m) {
            auto& item = dets[m];
            count++;
            res.push_back(item);
            for (size_t n = m + 1; n < dets.size(); ++n) {
                if (iou(item.bbox, dets[n].bbox) > nms_thresh) {
                    dets.erase(dets.begin()+n);
                    --n;
                }
            }
        }
    }
    // std::cout<<"get "<<count<<" after nms.."<<std::endl;
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/* This is a sample bounding box parsing function for the sample YoloV5m detector model */
static bool NvDsInferParseYoloV5(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList)
{
    
    std::vector<Detection> res;
    std::vector<Detection> det;
    // test((float*)(outputLayersInfo[0].buffer));

    decodeandnms(res, (float*)(outputLayersInfo[0].buffer), CONF_THRESH, NMS_THRESH);
    // nms(res, det, NMS_THRESH, networkInfo.width, networkInfo.height);
    
    for(auto& r : res) {
	    NvDsInferParseObjectInfo oinfo;        
        
	    oinfo.classId = r.class_id;
	    oinfo.left    = static_cast<unsigned int>(r.bbox[0]);
	    oinfo.top     = static_cast<unsigned int>(r.bbox[1]);
	    oinfo.width   = static_cast<unsigned int>(r.bbox[2] - r.bbox[0]);
	    oinfo.height  = static_cast<unsigned int>(r.bbox[3] - r.bbox[1]);
	    oinfo.detectionConfidence = r.conf;
        // std::cout<<r.bbox[0]<<" "<<r.bbox[1]<<" "<<r.bbox[2]<<" "<<r.bbox[3]<<" "<<"  "<<r.conf<<"  "<<r.class_id<<std::endl;
        // std::cout<<oinfo.left<<" "<<oinfo.top<<std::endl;

	    objectList.push_back(oinfo);        
    }
    
    return true;
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/* C-linkage to prevent name-mangling */
extern "C" bool NvDsInferParseCustomYoloV5(
    std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo,
    NvDsInferParseDetectionParams const &detectionParams,
    std::vector<NvDsInferParseObjectInfo> &objectList)
{
    return NvDsInferParseYoloV5(
        outputLayersInfo, networkInfo, detectionParams, objectList);
}


/* Check that the custom function has been defined correctly */
CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE(NvDsInferParseCustomYoloV5);

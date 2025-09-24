#pragma once
#include <iostream>
#include <vector>
#include <fstream>
#include "../LikesProgram/Math/PercentileSketch.hpp"

namespace PercentileSketchTest {
	void Test() {
        // 创建百分位估算器
        LikesProgram::Math::PercentileSketch sketch1(1200, 2);

        // 单个数据添加
        for (double x = 0; x < 500; x += 1.0) {
            sketch1.Add(x);
        }

        // 批量添加数据
        std::vector<double> batchData;
        for (double x = 500; x < 1000; x += 1.0) batchData.push_back(x);
        sketch1.AddBatch(batchData);

        // 压缩以优化查询和内存
        sketch1.Compress();

        // 查询百分位
        std::cout << "50th percentile: " << sketch1.Quantile(0.5) << std::endl;
        std::cout << "90th percentile: " << sketch1.Quantile(0.9) << std::endl;
        std::cout << "99th percentile: " << sketch1.Quantile(0.99) << std::endl;

        // 获取质心信息
        auto centroids = sketch1.GetCentroids();
        std::cout << "Centroid count: " << centroids.size() << std::endl;
        for (size_t i = 0; i < std::min((size_t)5, centroids.size()); ++i) {
            std::cout << "Mean: " << centroids[i].first << ", Count: " << centroids[i].second << std::endl;
        }

        // 创建另一个 sketch 并添加数据
        LikesProgram::Math::PercentileSketch sketch2(800, 2);
        for (double x = 1000; x < 1500; x += 1.0) sketch2.Add(x);
        sketch2.Compress();  // 压缩后再 merge，提高精度

        // 合并另一个 PercentileSketch
        sketch1.Merge(sketch2);

        // 再次压缩 merge 后的 sketch，提高尾部分位精度
        sketch1.Compress();

        std::cout << "After merge, 95th percentile: " << sketch1.Quantile(0.95) << std::endl;

        // 序列化
        std::ofstream ofs("sketch.bin", std::ios::binary);
        sketch1.Serialize(ofs);
        ofs.close();

        // 反序列化
        std::ifstream ifs("sketch.bin", std::ios::binary);
        auto loadedSketch = LikesProgram::Math::PercentileSketch::Deserialize(ifs);
        ifs.close();

        std::cout << "Deserialized 50th percentile: " << loadedSketch.Quantile(0.5) << std::endl;
	}
}
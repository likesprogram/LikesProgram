#pragma once
#include <iostream>
#include <vector>
#include <fstream>
#include "../LikesProgram/Math/PercentileSketch.hpp"

namespace PercentileSketchTest {
	void Test() {
        // �����ٷ�λ������
        LikesProgram::Math::PercentileSketch sketch1(1200, 2);

        // �����������
        for (double x = 0; x < 500; x += 1.0) {
            sketch1.Add(x);
        }

        // �����������
        std::vector<double> batchData;
        for (double x = 500; x < 1000; x += 1.0) batchData.push_back(x);
        sketch1.AddBatch(batchData);

        // ѹ�����Ż���ѯ���ڴ�
        sketch1.Compress();

        // ��ѯ�ٷ�λ
        std::cout << "50th percentile: " << sketch1.Quantile(0.5) << std::endl;
        std::cout << "90th percentile: " << sketch1.Quantile(0.9) << std::endl;
        std::cout << "99th percentile: " << sketch1.Quantile(0.99) << std::endl;

        // ��ȡ������Ϣ
        auto centroids = sketch1.GetCentroids();
        std::cout << "Centroid count: " << centroids.size() << std::endl;
        for (size_t i = 0; i < std::min((size_t)5, centroids.size()); ++i) {
            std::cout << "Mean: " << centroids[i].first << ", Count: " << centroids[i].second << std::endl;
        }

        // ������һ�� sketch ���������
        LikesProgram::Math::PercentileSketch sketch2(800, 2);
        for (double x = 1000; x < 1500; x += 1.0) sketch2.Add(x);
        sketch2.Compress();  // ѹ������ merge����߾���

        // �ϲ���һ�� PercentileSketch
        sketch1.Merge(sketch2);

        // �ٴ�ѹ�� merge ��� sketch�����β����λ����
        sketch1.Compress();

        std::cout << "After merge, 95th percentile: " << sketch1.Quantile(0.95) << std::endl;

        // ���л�
        std::ofstream ofs("sketch.bin", std::ios::binary);
        sketch1.Serialize(ofs);
        ofs.close();

        // �����л�
        std::ifstream ifs("sketch.bin", std::ios::binary);
        auto loadedSketch = LikesProgram::Math::PercentileSketch::Deserialize(ifs);
        ifs.close();

        std::cout << "Deserialized 50th percentile: " << loadedSketch.Quantile(0.5) << std::endl;
	}
}
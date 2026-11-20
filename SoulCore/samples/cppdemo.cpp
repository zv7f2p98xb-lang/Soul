// Soul örnek programı: C++ (STL, sanal işlevler, lambdalar)
#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <vector>
struct Hayvan { virtual ~Hayvan() = default; virtual std::string ses() const = 0; virtual std::string ad() const = 0; };
struct Kedi : Hayvan { std::string ses() const override { return "Miyav"; } std::string ad() const override { return "Kedi"; } };
struct Kopek : Hayvan { std::string ses() const override { return "Hav hav"; } std::string ad() const override { return "Köpek"; } };
int main() {
    std::vector<std::unique_ptr<Hayvan>> h;
    h.push_back(std::make_unique<Kedi>());
    h.push_back(std::make_unique<Kopek>());
    for (const auto &x : h) std::cout << x->ad() << " diyor ki: " << x->ses() << "\n";
    std::vector<int> v(20);
    std::iota(v.begin(), v.end(), 1);
    long long kare = std::accumulate(v.begin(), v.end(), 0LL, [](long long a, int b) { return a + (long long)b * b; });
    std::cout << "1..20 karelerinin toplamı = " << kare << "\n";
    std::map<char, int> harf;
    std::string metin = "soul ipad uzerinde windows programlari calistirir";
    for (char c : metin) if (c != ' ') harf[c]++;
    auto enCok = std::max_element(harf.begin(), harf.end(), [](auto &a, auto &b) { return a.second < b.second; });
    std::cout << "En sık harf: '" << enCok->first << "' (" << enCok->second << " kez)\n";
    return 0;
}

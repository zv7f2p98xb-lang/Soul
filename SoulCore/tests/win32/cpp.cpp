#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <memory>
#include <sstream>
struct Shape { virtual ~Shape() {} virtual double area() const = 0; virtual std::string name() const = 0; };
struct Circle : Shape { double r; Circle(double r) : r(r) {} double area() const override { return 3.14159265 * r * r; } std::string name() const override { return "circle"; } };
struct Rect : Shape { double w, h; Rect(double w, double h) : w(w), h(h) {} double area() const override { return w * h; } std::string name() const override { return "rect"; } };
int main() {
    std::vector<std::unique_ptr<Shape>> shapes;
    shapes.emplace_back(new Circle(1.5));
    shapes.emplace_back(new Rect(2, 3));
    shapes.emplace_back(new Circle(0.5));
    double total = 0;
    for (auto &s : shapes) { std::cout << s->name() << " " << s->area() << "\n"; total += s->area(); }
    std::cout << "total=" << total << std::endl;
    std::map<std::string, int> counts;
    std::string text = "elma armut elma kiraz armut elma";
    std::istringstream iss(text);
    std::string w;
    while (iss >> w) counts[w]++;
    for (auto &kv : counts) std::cout << kv.first << ":" << kv.second << " ";
    std::cout << "\n";
    std::vector<int> v = { 5, 3, 9, 1, 7 };
    std::sort(v.begin(), v.end(), [](int a, int b) { return a > b; });
    for (int x : v) std::cout << x << ",";
    std::cout << "\n";
    std::string s = "Soul";
    s += " on iPad";
    std::reverse(s.begin(), s.end());
    std::cout << s << " len=" << s.size() << std::endl;
    return 0;
}

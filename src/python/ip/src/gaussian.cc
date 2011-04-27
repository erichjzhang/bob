/**
 * @file src/python/ip/src/DCTFeatures.cc
 * @author <a href="mailto:Nikls.Johansson@idiap.ch">Niklas Johansson</a> 
 * @date Thu 17 Mar 19:12:40 2011 
 *
 * @brief Binds Gaussian smoothing to python
 */

#include <boost/python.hpp>
#include <vector>
#include "ip/gaussian.h"

using namespace boost::python;
namespace ip = Torch::ip;

static const char* gaussiandoc = "Performs gaussian smoothing";

void bind_ip_gaussian() {
	class_<ip::GaussianSmooth, boost::shared_ptr<ip::GaussianSmooth> >("GaussianSmooth", gaussiandoc, init<const int, const int, const double>((arg("radius_x"), arg("radius_y"), arg("sigma")="0.25"), "Create a gaussian smoother"))
		.def("__call__", &GaussianSmooth.operator<uint8_t>, (arg("self"), arg("src"), arg("dst")), "Smooth an image")
		.def("__call__", &GaussianSmooth.operator<uint16_t>, (arg("self"), arg("src"), arg("dst")), "Smooth an image")
		;
}


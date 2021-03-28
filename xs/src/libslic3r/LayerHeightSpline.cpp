/*
 * This class represents a set of layers and their heights.
 * It is intended for smoothing the height distribution (avoid very thin
 * layers next to thick layers) and to correctly interpolate higher layers if
 * a layer height changes somewhere in a lower position at the object.
 * Uses http://www.eol.ucar.edu/homes/granger/bspline/doc/ for spline computation.
 */

#include "LayerHeightSpline.hpp"
#include <cmath> // std::abs

namespace Slic3r {

LayerHeightSpline::LayerHeightSpline()
:   _object_height(0)
{
    this->_is_valid = false;
    this->_layers_updated = false;
    this->_layer_heights_updated = false;
}

LayerHeightSpline::LayerHeightSpline(const LayerHeightSpline &other)
{
    *this = other;
}

LayerHeightSpline& LayerHeightSpline::operator=(const LayerHeightSpline &other)
{
    this->_object_height = other._object_height;
    this->_layers = other._layers;
    this->_layer_heights = other._layer_heights;
    this->_is_valid = other._is_valid;
    this->_layers_updated = other._layers_updated;
    this->_layer_heights_updated = other._layer_heights_updated;
    if(this->_is_valid) {
        this->_updateBSpline();
    }
    return *this;
}

/*
 * Indicates whether the object has valid data and the spline was successfully computed or not.
 */
bool LayerHeightSpline::hasData()
{
    return this->_is_valid;
}

/*
 * Set absolute layer positions in object coordinates.
 * Heights (thickness of each layer) is generated from this list.
 */
bool LayerHeightSpline::setLayers(std::vector<coordf_t> layers)
{
    this->_layers = layers;

    // generate updated layer height list from layers
    this->_layer_heights.clear();
    coordf_t last_z = 0;
    for (std::vector<coordf_t>::const_iterator l = this->_layers.begin(); l != this->_layers.end(); ++l) {
        this->_layer_heights.push_back(*l-last_z);
        last_z = *l;
    }

    this->_layers_updated = true;
    this->_layer_heights_updated = false;

    return this->_updateBSpline();
}


/*
 * Update only the desired thickness of the layers, but not their positions!
 * This modifies the y-values for the spline computation and only affects
 * the resulting layers which can be obtained with getInterpolatedLayers.
 * The argument vector must be of the same size as the layers vector.
 */
bool LayerHeightSpline::updateLayerHeights(std::vector<coordf_t> heights)
{
    bool result = false;

    // do we receive the correct number of values?
    if(heights.size() == this->_layers.size()) {
        this->_layer_heights = heights;
        result = this->_updateBSpline();
    }else{
        std::cout << "Unable to update layer heights. You provided " << heights.size() << " layers, but " << this->_layers.size()-1 << " expected" << std::endl;
    }

    this->_layers_updated = false;
    this->_layer_heights_updated = true;

    return result;
}

/*
 * Reset the this object, remove database and interpolated results.
 */
void LayerHeightSpline::clear()
{
    this->_layers.clear();
    this->_layer_heights.clear();
    this->_layer_height_spline.reset();
    this->_is_valid = false;
    this->_layers_updated = false;
    this->_layer_heights_updated = false;
}


/*
 * Get a full set of layer z-positions by interpolation along the spline.
 */
std::vector<coordf_t> LayerHeightSpline::getInterpolatedLayers() const
{
    std::vector<coordf_t> layers;
    if(this->_is_valid) {
        // preserve first layer for bed contact
        layers.push_back(this->_layers[0]);
        coordf_t z = this->_layers[0];
        coordf_t h;
        coordf_t h_diff = 0;
        coordf_t last_h_diff = 0;
        coordf_t eps = 0.0001;
        while(z <= this->_object_height) {
            h = 0;
            h_diff = 0;
            // find intersection between layer height and spline
            do {
                last_h_diff = h_diff;
                h += h_diff/2;
                h = this->_layer_height_spline->evaluate(z+h);
                h_diff = this->_layer_height_spline->evaluate(z+h) - h;
            } while(std::abs(h_diff) > eps && std::abs(h_diff - last_h_diff) > eps);

            if(z+h > this->_object_height) {
                z += this->_layer_height_spline->evaluate(layers.back()); // re-use last layer height if outside of defined range
            }else{
                z += h;
            }
            layers.push_back(z);
        }
        // how to make sure, the last layer is not higher than object while maintaining between min/max layer height?
    }
    return layers;
}

/*
 * Evaluate interpolated layer height (thickness) at given z-position
 */
const coordf_t LayerHeightSpline::getLayerHeightAt(coordf_t height)
{
    coordf_t result = 0;
    if (this->_is_valid) {
        if(height <= this->_layers[0]) {
            result = this->_layers[0]; // return first_layer height
        }else if (height > this->_layers.back()){
            result = this->_layer_height_spline->evaluate(this->_layers.back()); // repeat last value for height > last layer
        }else{
            result = this->_layer_height_spline->evaluate(height); // return interpolated layer height
        }
    }
    return result;
}

/*
 * Internal method to re-compute the spline
 */
bool LayerHeightSpline::_updateBSpline()
{
    bool result = false;
    //TODO: exception if not enough points?

    // copy layer vectors and duplicate a datapoint at the front / end to achieve correct boundary conditions
    this->_spline_layers = this->_layers;
    this->_spline_layers[0] = 0;
    this->_spline_layers.push_back(this->_spline_layers.back()+1);

    this->_spline_layer_heights = this->_layer_heights;

    if (this->_layer_heights.size() > 1)
        this->_spline_layer_heights.at(0) = this->_spline_layer_heights.at(1); // override fixed first layer height with first "real" layer
    this->_spline_layer_heights.push_back(this->_spline_layer_heights.back());

    this->_layer_height_spline.reset(new BSpline<double>(&this->_spline_layers.at(0),
            this->_spline_layers.size(),
            &this->_spline_layer_heights.at(0),
            0,
            1,
            0)
    );

    if (this->_layer_height_spline->ok()) {
        result = true;
    } else {
        result = false;
        std::cout << "Spline setup failed." << std::endl;
    }

    this->_is_valid = result;

    return result;
}


}

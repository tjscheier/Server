/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
*
* This file is part of CasparCG (www.casparcg.com).
*
* CasparCG is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CasparCG is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
*
* Author: Robert Nagy, ronag89@gmail.com
*/

#include "../StdAfx.h"

#include "stage.h"

#include "layer.h"

#include "../frame/draw_frame.h"
#include "../frame/frame_factory.h"

#include <common/concurrency/executor.h>
#include <common/diagnostics/graph.h>

#include <core/frame/frame_transform.h>

#include <boost/foreach.hpp>
#include <boost/timer.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/range/algorithm_ext.hpp>

#include <tbb/parallel_for_each.h>

#include <map>
#include <vector>

namespace caspar { namespace core {
	
struct stage::impl : public std::enable_shared_from_this<impl>
{			
	std::map<int, layer>				layers_;	
	std::map<int, tweened_transform>	transforms_;	
	executor							executor_;
public:
	impl() : executor_(L"stage")
	{
	}
		
	std::map<int, safe_ptr<draw_frame>> operator()(const struct video_format_desc& format_desc)
	{		
		return executor_.invoke([=]() -> std::map<int, safe_ptr<draw_frame>>
		{
			std::map<int, safe_ptr<class draw_frame>> frames;

			try
			{					
				BOOST_FOREACH(auto& layer, layers_)			
					frames[layer.first] = draw_frame::empty();	

				auto format_desc2 = format_desc;

				tbb::parallel_for_each(layers_.begin(), layers_.end(), [&](std::map<int, layer>::value_type& layer)
				{
					auto transform = transforms_[layer.first].fetch_and_tick(1);

					int flags = frame_producer::flags::none;
					if(format_desc2.field_mode != field_mode::progressive)
					{
						flags |= std::abs(transform.fill_scale[1]  - 1.0) > 0.0001 ? frame_producer::flags::deinterlace : frame_producer::flags::none;
						flags |= std::abs(transform.fill_translation[1])  > 0.0001 ? frame_producer::flags::deinterlace : frame_producer::flags::none;
					}

					if(transform.is_key)
						flags |= frame_producer::flags::alpha_only;

					auto frame = layer.second.receive(flags);	
				
					auto frame1 = make_safe<core::draw_frame>(frame);
					frame1->get_frame_transform() = transform;

					if(format_desc2.field_mode != core::field_mode::progressive)
					{				
						auto frame2 = make_safe<core::draw_frame>(frame);
						frame2->get_frame_transform() = transforms_[layer.first].fetch_and_tick(1);
						frame1 = core::draw_frame::interlace(frame1, frame2, format_desc2.field_mode);
					}

					frames[layer.first] = frame1;
				});		
			}
			catch(...)
			{
				layers_.clear();
				CASPAR_LOG_CURRENT_EXCEPTION();
			}	

			return frames;
		});
	}
		
	void apply_transforms(const std::vector<std::tuple<int, stage::transform_func_t, unsigned int, tweener>>& transforms)
	{
		executor_.begin_invoke([=]
		{
			BOOST_FOREACH(auto& transform, transforms)
			{
				auto src = transforms_[std::get<0>(transform)].fetch();
				auto dst = std::get<1>(transform)(src);
				transforms_[std::get<0>(transform)] = tweened_transform(src, dst, std::get<2>(transform), std::get<3>(transform));
			}
		}, high_priority);
	}
						
	void apply_transform(int index, const stage::transform_func_t& transform, unsigned int mix_duration, const tweener& tween)
	{
		executor_.begin_invoke([=]
		{
			auto src = transforms_[index].fetch();
			auto dst = transform(src);
			transforms_[index] = tweened_transform(src, dst, mix_duration, tween);
		}, high_priority);
	}

	void clear_transforms(int index)
	{
		executor_.begin_invoke([=]
		{
			transforms_.erase(index);
		}, high_priority);
	}

	void clear_transforms()
	{
		executor_.begin_invoke([=]
		{
			transforms_.clear();
		}, high_priority);
	}
		
	void load(int index, const safe_ptr<frame_producer>& producer, bool preview, int auto_play_delta)
	{
		executor_.begin_invoke([=]
		{
			layers_[index].load(producer, preview, auto_play_delta);
		}, high_priority);
	}

	void pause(int index)
	{		
		executor_.begin_invoke([=]
		{
			layers_[index].pause();
		}, high_priority);
	}

	void play(int index)
	{		
		executor_.begin_invoke([=]
		{
			layers_[index].play();
		}, high_priority);
	}

	void stop(int index)
	{		
		executor_.begin_invoke([=]
		{
			layers_[index].stop();
		}, high_priority);
	}

	void clear(int index)
	{
		executor_.begin_invoke([=]
		{
			layers_.erase(index);
		}, high_priority);
	}
		
	void clear()
	{
		executor_.begin_invoke([=]
		{
			layers_.clear();
		}, high_priority);
	}	
		
	void swap_layers(const safe_ptr<stage>& other)
	{
		if(other->impl_.get() == this)
			return;
		
		auto func = [=]
		{
			std::swap(layers_, other->impl_->layers_);
		};		
		executor_.begin_invoke([=]
		{
			other->impl_->executor_.invoke(func, high_priority);
		}, high_priority);
	}

	void swap_layer(int index, int other_index)
	{
		executor_.begin_invoke([=]
		{
			std::swap(layers_[index], layers_[other_index]);
		}, high_priority);
	}

	void swap_layer(int index, int other_index, const safe_ptr<stage>& other)
	{
		if(other->impl_.get() == this)
			swap_layer(index, other_index);
		else
		{
			auto func = [=]
			{
				std::swap(layers_[index], other->impl_->layers_[other_index]);
			};		
			executor_.begin_invoke([=]
			{
				other->impl_->executor_.invoke(func, high_priority);
			}, high_priority);
		}
	}
		
	boost::unique_future<safe_ptr<frame_producer>> foreground(int index)
	{
		return executor_.begin_invoke([=]
		{
			return layers_[index].foreground();
		}, high_priority);
	}
	
	boost::unique_future<safe_ptr<frame_producer>> background(int index)
	{
		return executor_.begin_invoke([=]
		{
			return layers_[index].background();
		}, high_priority);
	}

	boost::unique_future<boost::property_tree::wptree> info()
	{
		return std::move(executor_.begin_invoke([this]() -> boost::property_tree::wptree
		{
			boost::property_tree::wptree info;
			BOOST_FOREACH(auto& layer, layers_)			
				info.add_child(L"layers.layer", layer.second.info())
					.add(L"index", layer.first);	
			return info;
		}, high_priority));
	}

	boost::unique_future<boost::property_tree::wptree> info(int index)
	{
		return std::move(executor_.begin_invoke([=]() -> boost::property_tree::wptree
		{
			return layers_[index].info();
		}, high_priority));
	}		
};

stage::stage() : impl_(new impl()){}
void stage::apply_transforms(const std::vector<stage::transform_tuple_t>& transforms){impl_->apply_transforms(transforms);}
void stage::apply_transform(int index, const std::function<core::frame_transform(core::frame_transform)>& transform, unsigned int mix_duration, const tweener& tween){impl_->apply_transform(index, transform, mix_duration, tween);}
void stage::clear_transforms(int index){impl_->clear_transforms(index);}
void stage::clear_transforms(){impl_->clear_transforms();}
void stage::load(int index, const safe_ptr<frame_producer>& producer, bool preview, int auto_play_delta){impl_->load(index, producer, preview, auto_play_delta);}
void stage::pause(int index){impl_->pause(index);}
void stage::play(int index){impl_->play(index);}
void stage::stop(int index){impl_->stop(index);}
void stage::clear(int index){impl_->clear(index);}
void stage::clear(){impl_->clear();}
void stage::swap_layers(const safe_ptr<stage>& other){impl_->swap_layers(other);}
void stage::swap_layer(int index, int other_index){impl_->swap_layer(index, other_index);}
void stage::swap_layer(int index, int other_index, const safe_ptr<stage>& other){impl_->swap_layer(index, other_index, other);}
boost::unique_future<safe_ptr<frame_producer>> stage::foreground(int index) {return impl_->foreground(index);}
boost::unique_future<safe_ptr<frame_producer>> stage::background(int index) {return impl_->background(index);}
boost::unique_future<boost::property_tree::wptree> stage::info() const{return impl_->info();}
boost::unique_future<boost::property_tree::wptree> stage::info(int index) const{return impl_->info(index);}
std::map<int, safe_ptr<class draw_frame>> stage::operator()(const video_format_desc& format_desc){return (*impl_)(format_desc);}
}}
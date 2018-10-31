//*****************************************************************************
// Copyright 2017-2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#include <algorithm>
#include <iostream>
#include <numeric>
#include <typeindex>
#include <typeinfo>
#include <unordered_set>

#include "cpu_rnn_fusion.hpp"
#include "ngraph/descriptor/output.hpp"
#include "ngraph/graph_util.hpp"
#include "ngraph/log.hpp"
#include "ngraph/op/add.hpp"
#include "ngraph/op/broadcast.hpp"
#include "ngraph/op/concat.hpp"
#include "ngraph/op/constant.hpp"
#include "ngraph/op/divide.hpp"
#include "ngraph/op/dot.hpp"
#include "ngraph/op/exp.hpp"
#include "ngraph/op/get_output_element.hpp"
#include "ngraph/op/multiply.hpp"
#include "ngraph/op/negative.hpp"
#include "ngraph/op/parameter.hpp"
#include "ngraph/op/relu.hpp"
#include "ngraph/op/reshape.hpp"
#include "ngraph/op/result.hpp"
#include "ngraph/op/slice.hpp"
#include "ngraph/op/sum.hpp"
#include "ngraph/op/tanh.hpp"
#include "ngraph/pattern/matcher.hpp"
#include "ngraph/pattern/op/label.hpp"
#include "ngraph/pattern/op/skip.hpp"
#include "ngraph/runtime/cpu/op/lstm.hpp"
#include "ngraph/runtime/cpu/op/rnn.hpp"
#include "ngraph/runtime/cpu/op/sigmoid.hpp"

using namespace ngraph;
void ngraph::runtime::cpu::pass::LSTMFusion::construct_sigmoid()
{
    // construct variance
    auto input = std::make_shared<pattern::op::Label>(element::f32, Shape{3, 4});
    auto neg_input = std::make_shared<op::Negative>(input);
    auto exp_neg_input = std::make_shared<op::Exp>(neg_input);

    // broadcast input
    auto constant = std::make_shared<pattern::op::Label>(element::f32, Shape{});
    auto broadcast_constant = std::make_shared<op::Broadcast>(constant, Shape{3, 4}, AxisSet{0, 1});

    auto add_exp = std::make_shared<op::Add>(exp_neg_input, broadcast_constant);
    auto divide_1_over_exp = std::make_shared<op::Divide>(broadcast_constant, add_exp);

    // Define a call back that needs to called once the DFG matches the pattern
    ngraph::pattern::graph_rewrite_callback callback = [input](pattern::Matcher& m) {
        NGRAPH_DEBUG << "In a callback for construct_fprop_sigmoid pattern against "
                     << m.get_match_root()->get_name();
        auto pattern_map = m.get_pattern_map();

        if (m.get_match_root()->get_element_type() != element::f32)
        {
            NGRAPH_DEBUG << "mpattern = " << m.get_match_root()->get_name()
                         << " type is not float!";
            return false;
        }

        if (m.get_match_root()->get_outputs().size() != pattern_map[input]->get_outputs().size())
        {
            NGRAPH_DEBUG << "mpattern = " << m.get_match_root()->get_name()
                         << "input= " << pattern_map[input]->get_name() << "size dont match!";
            return false;
        }

        auto sigmoid_node = std::make_shared<op::Sigmoid>(pattern_map[input]);
        ngraph::replace_node(m.get_match_root(), sigmoid_node);
        return true;
    };

    auto m = std::make_shared<ngraph::pattern::Matcher>(divide_1_over_exp, callback);
    this->add_matcher(m);
}

void ngraph::runtime::cpu::pass::LSTMFusion::construct_lstm_fprop()
{
    auto input_xt = std::make_shared<pattern::op::Label>(element::f32, Shape{10, 100});
    auto weights_i2h = std::make_shared<pattern::op::Label>(
        element::f32, Shape{400, 100}, pattern::has_class<op::Parameter>());
    auto weights_i2h_reshape =
        std::make_shared<op::Reshape>(weights_i2h, AxisVector{1, 0}, Shape{100, 400});
    auto dot_1 = std::make_shared<op::Dot>(input_xt, weights_i2h_reshape);

    auto broadcast_pred = [](std::shared_ptr<Node> n) {
        return ((std::dynamic_pointer_cast<op::Broadcast>(n) != nullptr) ||
                (std::dynamic_pointer_cast<op::Reshape>(n) != nullptr));
    };

    auto bias_i2h = std::make_shared<pattern::op::Label>(
        element::f32, Shape{10, 400}, pattern::has_class<op::Parameter>());
    auto skip_broadcast_i2h = std::make_shared<pattern::op::Skip>(bias_i2h, broadcast_pred);
    auto add_1 = std::make_shared<op::Add>(dot_1, skip_broadcast_i2h);

    auto hidden_ht = std::make_shared<pattern::op::Label>(element::f32, Shape{10, 50});
    auto weights_h2h = std::make_shared<pattern::op::Label>(
        element::f32, Shape{400, 50}, pattern::has_class<op::Parameter>());
    auto param2_2_reshape =
        std::make_shared<op::Reshape>(weights_h2h, AxisVector{1, 0}, Shape{50, 400});
    auto dot_2 = std::make_shared<op::Dot>(hidden_ht, param2_2_reshape);
    auto bias_h2h = std::make_shared<pattern::op::Label>(
        element::f32, Shape{10, 400}, pattern::has_class<op::Parameter>());
    auto skip_broadcast_h2h = std::make_shared<pattern::op::Skip>(bias_h2h, broadcast_pred);
    auto add_2 = std::make_shared<op::Add>(dot_2, skip_broadcast_h2h);

    auto X = std::make_shared<op::Add>(add_2, add_1);
    // construct forget gate
    auto input_slice_0 = std::make_shared<op::Slice>(X, Coordinate{0, 0}, Coordinate{10, 100});
    auto forget_gate = std::make_shared<op::Sigmoid>(input_slice_0);

    // ct-1 -> cell state (src_iter -> {ht | ct-1}
    auto ct_1 = std::make_shared<pattern::op::Label>(element::f32, Shape{10, 100});
    auto multiply_forget_gate_ct_1 = std::make_shared<op::Multiply>(forget_gate, ct_1);

    // construct input gate
    auto input_slice_1 = std::make_shared<op::Slice>(X, Coordinate{0, 100}, Coordinate{10, 200});
    auto input_gate = std::make_shared<op::Sigmoid>(input_slice_1);
    auto input_slice_2 = std::make_shared<op::Slice>(X, Coordinate{0, 200}, Coordinate{10, 300});
    auto tanh_1 = std::make_shared<op::Tanh>(input_slice_2);
    auto multiply_input_gate_tanh_1 = std::make_shared<op::Multiply>(input_gate, tanh_1);

    auto add_ct_1_input_gate_tanh_1 =
        std::make_shared<op::Add>(multiply_forget_gate_ct_1, multiply_input_gate_tanh_1);
    auto ct_label = std::make_shared<pattern::op::Label>(
        add_ct_1_input_gate_tanh_1, nullptr, NodeVector{add_ct_1_input_gate_tanh_1});

    // construct output gate
    auto input_slice_3 = std::make_shared<op::Slice>(X, Coordinate{0, 300}, Coordinate{10, 400});
    auto output_gate = std::make_shared<op::Sigmoid>(input_slice_3);
    auto tanh_2 = std::make_shared<op::Tanh>(ct_label);
    auto ht = std::make_shared<op::Multiply>(output_gate, tanh_2);
    auto ht_label = std::make_shared<pattern::op::Label>(ht, nullptr, NodeVector{ht});

    // Define a call back that needs to called once the DFG matches the pattern
    pattern::graph_rewrite_callback callback =
        [ct_label, input_xt, weights_i2h, hidden_ht, weights_h2h, bias_i2h, bias_h2h, ct_1](
            pattern::Matcher& m) {
            static int count = 0;
            NGRAPH_DEBUG << "In a callback for construct_fprop_lstm pattern against "
                         << m.get_match_root()->get_name();

            auto pattern_map = m.get_pattern_map();
            NGRAPH_DEBUG << "In Lstm fprop call back";

            if (m.get_match_root()->get_element_type() != element::f32)
            {
                NGRAPH_DEBUG << "mpattern = " << m.get_match_root()->get_name()
                             << " type is not float!";
                return false;
            }
            auto input_xt_rank = pattern_map[input_xt]->get_shape().size();
            auto hidden_ht_rank = pattern_map[hidden_ht]->get_shape().size();
            auto weights_i2h_rank = pattern_map[weights_i2h]->get_shape().size();
            auto weights_h2h_rank = pattern_map[weights_h2h]->get_shape().size();
            if (input_xt_rank != 2 || hidden_ht_rank != 2 || weights_i2h_rank != 2 ||
                weights_h2h_rank != 2)
            {
                return false;
            }

            if (pattern_map[bias_i2h]->get_shape().size() != 1 ||
                pattern_map[bias_h2h]->get_shape().size() != 1)
            {
                throw ngraph_error("Bias should have rank of 1 for MKLDNN Rnn op");
            }

            std::shared_ptr<Node> src_layer = pattern_map[input_xt];
            std::shared_ptr<Node> hidden_state_ht = pattern_map[hidden_ht];
            std::shared_ptr<Node> weights_layer = pattern_map[weights_i2h];
            std::shared_ptr<Node> weights_iter = pattern_map[weights_h2h];

            auto swap_lstm_inputs = [&]() -> void {
                src_layer = pattern_map[hidden_ht];
                hidden_state_ht = pattern_map[input_xt];
                weights_layer = pattern_map[weights_h2h];
                weights_iter = pattern_map[weights_i2h];
            };

            // determine which forms input/hidden inputs in the matched subgraph
            if ((std::dynamic_pointer_cast<op::Broadcast>(src_layer) &&
                 std::dynamic_pointer_cast<op::Constant>(src_layer->get_argument(0))) &&
                !std::dynamic_pointer_cast<op::GetOutputElement>(src_layer->get_argument(0)))
            {
                swap_lstm_inputs();
            }
            else if (hidden_state_ht->get_shape() != pattern_map[ct_1]->get_shape())
            {
                swap_lstm_inputs();
            }
            // swap the inputs if the cell_state and hidden state doesnot belong to the same Lstm
            else if (std::dynamic_pointer_cast<op::GetOutputElement>(
                         pattern_map[ct_1]->get_argument(0)))
            {
                if (hidden_state_ht->get_argument(0)->get_arguments()[0] !=
                    pattern_map[ct_1]->get_argument(0)->get_arguments()[0])
                {
                    swap_lstm_inputs();
                }
            }

            // get LSTM cell attributes
            size_t lstm_n_gates = 4;
            size_t batch_size = pattern_map[input_xt]->get_shape()[0];
            auto slc = weights_layer->get_shape()[1];
            auto dlc = weights_layer->get_shape()[0] / lstm_n_gates;
            auto sic = weights_iter->get_shape()[1];
            auto dic = weights_iter->get_shape()[0] / lstm_n_gates;

            if (dlc != dic)
            {
                NGRAPH_DEBUG
                    << "Not fusing, since MKLDNN Lstm kernel requires dst_layer feature size "
                       "equals to dts_iter feature size";
                return false;
            }

            auto replace_collapse_node_user = [](std::shared_ptr<Node> collapsed_node,
                                                 descriptor::Output& new_output) {
                for (auto node : collapsed_node->get_users())
                {
                    NGRAPH_DEBUG << "node_name: " << node->get_name();
                    for (size_t i = 0; i < node->get_input_size(); i++)
                    {
                        if (node->get_argument(i) == collapsed_node)
                        {
                            node->get_inputs().at(i).replace_output(new_output);
                        }
                    }
                }
            };

            std::shared_ptr<Node> src_iter =
                std::make_shared<op::Concat>(NodeVector{hidden_state_ht, pattern_map[ct_1]}, 0);
            std::shared_ptr<Node> bias =
                std::make_shared<op::Add>(pattern_map[bias_i2h], pattern_map[bias_h2h]);

            // Reorder weights_layer from ldgoi -> ldigo
            auto weights_layer_reorder = std::make_shared<op::Reshape>(
                weights_layer, AxisVector{1, 0}, Shape{slc, lstm_n_gates * dlc});
            // Reorder weights_iyer from ldgoi -> ldigo
            auto weights_iter_reorder = std::make_shared<op::Reshape>(
                weights_iter, AxisVector{1, 0}, Shape{sic, lstm_n_gates * dic});

            auto lstm_node = std::make_shared<op::Lstm>(
                src_layer, src_iter, weights_layer_reorder, weights_iter_reorder, bias);

            auto lstm_ht_output = std::make_shared<op::GetOutputElement>(lstm_node, 0);
            auto lstm_ht_ct_output = std::make_shared<op::GetOutputElement>(lstm_node, 1);
            // dst_iter of lstm mkldnn output holds the results of both recurrent state
            // tensor outputs. we need to slice the ct.
            auto ht_slice = std::make_shared<op::Slice>(
                lstm_ht_output, Coordinate{0, 0}, Coordinate{batch_size, dlc});
            auto ct_slice = std::make_shared<op::Slice>(
                lstm_ht_ct_output, Coordinate{batch_size, 0}, Coordinate{(2 * batch_size), dic});

            if (lstm_node->get_outputs().at(0).get_inputs().size() != 2)
            {
                throw ngraph_error("Lstm node doesnt have two outputs");
            }
            // Now identify the nodes which consumes the output of LSTM nodes
            // and replace them accordingly
            // find the user's for {ht|ct} and replace them with lstm_goe_1
            if (ngraph::is_used(pattern_map[ct_label].get()))
            {
                replace_collapse_node_user(pattern_map[ct_label], ct_slice->get_outputs().at(0));
            }
            // find the user's for {ht} and replace them with lstm_goe_0
            ngraph::replace_node(m.get_match_root(), ht_slice);
            return true;
        };
    auto m = std::make_shared<pattern::Matcher>(ht, callback);
    this->add_matcher(m);
}

void ngraph::runtime::cpu::pass::RNNFusion::construct_rnn_lstm_fprop()
{
    auto src_layer_label = std::make_shared<pattern::op::Label>(element::f32, Shape{30, 100});

    auto ht_label = std::make_shared<pattern::op::Label>(element::f32, Shape{10, 100});
    auto ct_label = std::make_shared<pattern::op::Label>(element::f32, Shape{10, 100});
    auto recurrent_inputs = std::make_shared<op::Concat>(NodeVector{ht_label, ct_label}, 0);
    auto src_iter_label = std::make_shared<pattern::op::Label>(
        recurrent_inputs, nullptr, NodeVector{recurrent_inputs});

    auto weights_i2h_param = std::make_shared<pattern::op::Label>(
        element::f32, Shape{400, 100}, pattern::has_class<op::Parameter>());
    auto weights_i2h_reshape =
        std::make_shared<op::Reshape>(weights_i2h_param, AxisVector{1, 0}, Shape{100, 400});
    auto weights_i2h_label = std::make_shared<pattern::op::Label>(
        weights_i2h_reshape, nullptr, NodeVector{weights_i2h_reshape});

    auto weights_h2h_param = std::make_shared<pattern::op::Label>(
        element::f32, Shape{400, 100}, pattern::has_class<op::Parameter>());
    auto weights_h2h_reshape =
        std::make_shared<op::Reshape>(weights_h2h_param, AxisVector{1, 0}, Shape{100, 400});
    auto weights_h2h_label = std::make_shared<pattern::op::Label>(
        weights_h2h_reshape, nullptr, NodeVector{weights_h2h_reshape});

    auto bias_i2h = std::make_shared<pattern::op::Label>(element::f32, Shape{400});
    auto bias_h2h = std::make_shared<pattern::op::Label>(element::f32, Shape{400});
    auto add_bias = std::make_shared<op::Add>(bias_i2h, bias_h2h);
    auto bias_label = std::make_shared<pattern::op::Label>(add_bias, nullptr, NodeVector{add_bias});

    auto lstm = std::make_shared<op::Lstm>(
        src_layer_label, src_iter_label, weights_i2h_label, weights_h2h_label, bias_label);
    auto lstm_goe = std::make_shared<op::GetOutputElement>(lstm, 0);
    auto lstm_goe_label =
        std::make_shared<pattern::op::Label>(lstm_goe, nullptr, NodeVector{lstm_goe});
    auto lstm_goe_slice =
        std::make_shared<op::Slice>(lstm_goe_label, Coordinate{10, 0}, Coordinate{20, 100});
    auto lstm_goe_slice_label =
        std::make_shared<pattern::op::Label>(lstm_goe_slice, nullptr, NodeVector{lstm_goe_slice});

    pattern::recurrent_graph_rewrite_callback callback = [ht_label,
                                                          ct_label,
                                                          weights_i2h_param,
                                                          weights_h2h_param,
                                                          bias_i2h,
                                                          bias_h2h,
                                                          lstm_goe_slice_label,
                                                          lstm_goe_label,
                                                          src_layer_label,
                                                          src_iter_label,
                                                          weights_i2h_label,
                                                          weights_h2h_label,
                                                          bias_label](
        pattern::RecurrentMatcher& m) {

        NGRAPH_DEBUG << " In recurrent RNN fusion callback";
        // this checks if all the matched lstm_nodes share the same weights and bias
        auto validate_rnn_inputs =
            [&](std::vector<std::shared_ptr<pattern::op::Label>> rnn_labels) -> bool {
            for (auto& label : rnn_labels)
            {
                auto bounded_nodes = m.get_bound_nodes_for_pattern(label);
                for (auto& rnn_input : bounded_nodes)
                {
                    if (rnn_input != bounded_nodes[0])
                    {
                        return false;
                    }
                }
            }
            return true;
        };
        auto concat_rnn_inputs_across_timestep =
            [&](std::shared_ptr<pattern::op::Label> input_label) -> std::shared_ptr<Node> {
            NodeVector concat_args;
            // src_layer -> concatenate input symbols from different LSTM cells belonging to same RNN layer
            // in the order 0, 1, 2... t time slice
            {
                auto node_labels = m.get_bound_nodes_for_pattern(input_label);
                if (node_labels.size() > 1)
                {
                    std::reverse(node_labels.begin(), node_labels.end());
                    return std::make_shared<op::Concat>(node_labels, 0);
                }
                else
                {
                    return node_labels[0];
                }
            }
        };

        if (!validate_rnn_inputs(std::vector<std::shared_ptr<pattern::op::Label>>{
                weights_i2h_param, weights_h2h_param, bias_i2h, bias_h2h}))
        {
            return false;
        }

        auto src_layer = concat_rnn_inputs_across_timestep(src_layer_label);
        auto src_iter_bounded_nodes = m.get_bound_nodes_for_pattern(src_iter_label);
        auto src_iter = src_iter_bounded_nodes[src_iter_bounded_nodes.size() - 1];
        if (!std::dynamic_pointer_cast<op::Concat>(src_iter))
        {
            return false;
        }

        auto weights_layer = m.get_bound_nodes_for_pattern(weights_i2h_label)[0];
        auto weights_iter = m.get_bound_nodes_for_pattern(weights_h2h_label)[0];
        auto bias = m.get_bound_nodes_for_pattern(bias_label)[0];

        auto num_of_lstm_matched = m.get_number_of_recurrent_matches();
        size_t lstm_n_gates = 4;
        // TODO: assert for batch_size, sequence length and num_of_lstm's fused
        size_t batch_size = src_layer->get_shape()[0] / num_of_lstm_matched;
        size_t sequence_len = num_of_lstm_matched;
        size_t src_layer_feature_size = weights_layer->get_shape()[0];
        size_t dlc = weights_layer->get_shape()[1] / lstm_n_gates;
        size_t dic = weights_iter->get_shape()[1] / lstm_n_gates;
        size_t src_iter_feature_size = weights_iter->get_shape()[0];
        // number of states for LSTM is 2
        size_t num_cell_states = 2;
        size_t direction = 1;
        size_t num_fused_rnn_layers = 1;

        NGRAPH_DEBUG << "src_layer: " << join(src_layer->get_shape());
        NGRAPH_DEBUG << "src_iter: " << join(src_iter->get_shape());
        NGRAPH_DEBUG << "weights_layer: " << join(weights_layer->get_shape());
        NGRAPH_DEBUG << "weights_iter: " << join(weights_iter->get_shape());
        NGRAPH_DEBUG << "bias: " << join(bias->get_shape());
        NGRAPH_DEBUG << "src_seq_len: " << sequence_len;
        NGRAPH_DEBUG << "batch_size: " << batch_size;
        NGRAPH_DEBUG << "src_iter_feature_size: " << src_iter_feature_size;

        // if we have have not found all the LSTM cells belonging to a layer
        // will return safely
        /*std::shared_ptr<Node> src_iter_arg = src_iter->get_arguments()[0];
        if (!(std::dynamic_pointer_cast<op::Broadcast>(src_iter_arg) &&
              std::dynamic_pointer_cast<op::Constant>(src_iter_arg->get_argument(0))))
        {
            return false;
        }*/
        if ((src_layer->get_shape()[0] / batch_size) != sequence_len &&
            !std::dynamic_pointer_cast<op::Parameter>(src_layer))
        {
            throw ngraph_error(
                "number of lstm inputs captured in the RNN fusion is not equal to "
                "src_sequence_length");
        }

        if (std::dynamic_pointer_cast<op::Parameter>(src_layer) && sequence_len != 1)
        {
            throw ngraph_error(
                "number of lstm inputs captured in the RNN fusion is not equal to "
                "src_sequence_length");
        }

        if ((src_iter->get_arguments().size()) != num_cell_states)
        {
            throw ngraph_error("number of states for RNN op is not equal to (ht_1|ct_1)");
        }

        auto src_layer_rank = src_layer->get_shape().size();
        auto src_iter_rank = src_iter->get_shape().size();
        auto weights_layer_rank = weights_layer->get_shape().size();
        auto weights_iter_rank = weights_iter->get_shape().size();
        auto bias_rank = bias->get_shape().size();
        if (src_layer_rank != 2 || src_iter_rank != 2 || weights_layer_rank != 2 ||
            weights_iter_rank != 2)
        {
            throw ngraph_error(
                "Pattern matcher error src_layer, src_iter, weights_layer, weighst_iter should "
                "have rank 2 for MKLDNN RNN op");
        }

        if (bias_rank != 1)
        {
            throw ngraph_error("Bias should have rank of 1 for MKLDNN Rnn op");
        }

        if (src_layer->get_element_type() != element::f32 ||
            src_iter->get_element_type() != element::f32)
        {
            throw ngraph_error(
                "input tensor type and input recurrent state tensor type for MKLDNN RNN op should "
                "be float32");
        }

        auto rnn = std::make_shared<op::Rnn>(src_layer,
                                             src_iter,
                                             weights_layer,
                                             weights_iter,
                                             bias,
                                             num_of_lstm_matched,
                                             lstm_n_gates,
                                             sequence_len,
                                             src_layer_feature_size,
                                             src_iter_feature_size,
                                             num_cell_states,
                                             direction,
                                             num_fused_rnn_layers);

        std::vector<std::shared_ptr<op::Slice>> ht_slice_per_timestep(num_of_lstm_matched, nullptr);
        auto rnn_ht_out = std::make_shared<op::GetOutputElement>(rnn, 0);
        auto rnn_ht_ct_out = std::make_shared<op::GetOutputElement>(rnn, 1);

        // slice the rnn ht's
        size_t start_index = 0;
        size_t end_index = batch_size;
        // capture the slices in the reverse order, so it corrosponds to lstm_goes order captured by the Pattern matcher
        for (size_t i = 0; i < num_of_lstm_matched; i++)
        {
            ht_slice_per_timestep[i] =
                (std::make_shared<op::Slice>(rnn_ht_out,
                                             Coordinate{start_index, 0},
                                             Coordinate{end_index, src_iter_feature_size}));
            start_index += batch_size;
            end_index += batch_size;
        }
        std::reverse(ht_slice_per_timestep.begin(), ht_slice_per_timestep.end());

        NGRAPH_DEBUG << "rnn_time_slice: " << ht_slice_per_timestep.size();

        // find the lstm's nodes captured in PM
        auto lstm_goes = m.get_bound_nodes_for_pattern(lstm_goe_label);
        std::vector<std::shared_ptr<ngraph::Node>> lstm_nodes;

        // we need to collect LSTM from GOE's, in order to deterministicaly determine
        // the individaual time slice output ht. lstm_goes will hold the GOE in the decreasing
        // order of the time slices
        for (size_t i = 0; i < lstm_goes.size(); i++)
        {
            // lstm's will be the input to GOE's
            lstm_nodes.push_back(lstm_goes[i]->get_arguments()[0]);
        }

        if (sequence_len != lstm_nodes.size())
        {
            throw ngraph_error(" Number of lstm nodes in RNN layer is not equal to time slices");
        }

        if (lstm_nodes.size() != lstm_goes.size() &&
            lstm_goes.size() != ht_slice_per_timestep.size())
        {
            throw ngraph_error(
                "Number of slices of rnn output ht is not equal to the time slices in RNN layer");
        }

        // collect all the consumers of LSTM goe's (ht)
        std::set<std::shared_ptr<ngraph::Node>> lstm_goe0_user;
        std::unordered_map<std::shared_ptr<Node>, std::shared_ptr<Node>>
            map_goe0_user_to_lstm_slices;
        std::shared_ptr<Node> goe_0;

        for (size_t index = 0; index < lstm_nodes.size(); index++)
        {
            // now get the GOE0 which is the first output of lstm (ht)
            for (auto& goes : lstm_nodes[index]->get_outputs().at(0).get_inputs())
            {
                auto goe_node = std::dynamic_pointer_cast<op::GetOutputElement>(goes->get_node());

                // if their is no GOE followed by the Lstm, their might be pattern match error
                // we will return safely
                if (!goe_node)
                {
                    return false;
                }
                // first output node of lstm
                if (goe_node && (goe_node->get_n() == 0))
                {
                    goe_0 = goes->get_node();
                    for (auto goe0_user : goe_0->get_users())
                    {
                        if (ngraph::is_used(goe0_user.get()))
                        {
                            lstm_goe0_user.insert(goe0_user);
                            map_goe0_user_to_lstm_slices.insert(
                                make_pair(goe0_user, ht_slice_per_timestep[index]));

                            NGRAPH_DEBUG << "ht_slice: " << ht_slice_per_timestep[index]->get_name()
                                         << " goe0_user " << goe0_user->get_name() << " ";
                        }
                    }
                }
                // we need to only check the last LSTM cell Ct user and replace if needed.
                if ((index == 0) && (goe_node->get_n() == 1))
                {
                    // dst_iter of lstm mkldnn output holds the results of both recurrent state
                    // tensor outputs. we need to slice the ct.
                    auto ct_slice = std::make_shared<op::Slice>(
                        rnn_ht_ct_out,
                        Coordinate{static_cast<unsigned long>(batch_size * direction *
                                                              num_fused_rnn_layers),
                                   0},
                        Coordinate{static_cast<unsigned long>(2 * batch_size * direction *
                                                              num_fused_rnn_layers),
                                   static_cast<unsigned long>(src_iter_feature_size)});

                    // check if the last LSTM cell has any consumers
                    auto last_lstmcell_goe1_user = goes->get_node()->get_users()[0];
                    if (ngraph::is_used(last_lstmcell_goe1_user.get()))
                    {
                        ngraph::replace_node(last_lstmcell_goe1_user, ct_slice);
                    }
                }
            }
        }

        // now go through the lstm goe_0 consumers and replace them with the slice
        for (auto& node : lstm_goe0_user)
        {
            if (ngraph::is_used(node.get()))
            {
                ngraph::replace_node(node, map_goe0_user_to_lstm_slices[node]);
            }
        }
        NGRAPH_DEBUG << "End of recurrent fusion call back "
                     << "matched_node: " << m.get_match_root()->get_name();
        std::cout << "Seq length " << sequence_len << std::endl;
        return true;

    };

    auto m = std::make_shared<pattern::RecurrentMatcher>(
        lstm_goe_slice_label,
        ct_label,
        std::set<std::shared_ptr<pattern::op::Label>>{
            weights_i2h_param, weights_h2h_param, bias_i2h, bias_h2h},
        callback);
    this->add_matcher(m);
}

static std::shared_ptr<Node>
    compute_multi_layer_rnn_inputs(const std::shared_ptr<pattern::op::Label>& rnn_label,
                                   pattern::RecurrentMatcher& m)
{
    auto node_labels = m.get_bound_nodes_for_pattern(rnn_label);
    std::reverse(node_labels.begin(), node_labels.end());
    return std::make_shared<op::Concat>(node_labels, 0);
}

void ngraph::runtime::cpu::pass::MultiLayerRNNFusion::construct_multi_layer_rnn_fusion_fprop()
{
    auto src_layer_label = std::make_shared<pattern::op::Label>(element::f32, Shape{30, 100});
    auto src_iter_label = std::make_shared<pattern::op::Label>(element::f32, Shape{20, 100});
    auto weights_layer_label = std::make_shared<pattern::op::Label>(element::f32, Shape{400, 100});
    auto weights_iter_label = std::make_shared<pattern::op::Label>(element::f32, Shape{400, 100});
    auto bias_label = std::make_shared<pattern::op::Label>(element::f32, Shape{400});

    size_t ref_number_of_timesteps = 3;
    size_t ref_number_of_gates_per_cell = 4;
    size_t ref_src_seq_length = 3;
    size_t ref_src_layer_feature_size = 100;
    size_t ref_feature_size = 100;
    size_t ref_num_rnn_cell_states = 2;
    size_t ref_rnn_direction = 1;
    size_t ref_num_of_rnn_fused_layer = 1;

    auto ref_rnn_node = std::make_shared<op::Rnn>(src_layer_label,
                                                  src_iter_label,
                                                  weights_layer_label,
                                                  weights_iter_label,
                                                  bias_label,
                                                  ref_number_of_timesteps,
                                                  ref_number_of_gates_per_cell,
                                                  ref_src_seq_length,
                                                  ref_src_layer_feature_size,
                                                  ref_feature_size,
                                                  ref_num_rnn_cell_states,
                                                  ref_rnn_direction,
                                                  ref_num_of_rnn_fused_layer);

    NodeVector ht_slice_per_timestep;
    auto rnn_ht_out = std::make_shared<op::GetOutputElement>(ref_rnn_node, 0);
    auto rnn_ht_label =
        std::make_shared<pattern::op::Label>(rnn_ht_out, nullptr, NodeVector{rnn_ht_out});
    auto rnn_ct_out = std::make_shared<op::GetOutputElement>(ref_rnn_node, 1);

    pattern::recurrent_graph_rewrite_callback callback = [src_layer_label,
                                                          src_iter_label,
                                                          weights_layer_label,
                                                          weights_iter_label,
                                                          bias_label,
                                                          rnn_ht_label](
        pattern::RecurrentMatcher& m) {
        if (m.get_number_of_recurrent_matches() <= 1)
        {
            return false;
        }

        auto src_nodes = m.get_bound_nodes_for_pattern(src_layer_label);
        auto rnn_ht_out_nodes = m.get_bound_nodes_for_pattern(rnn_ht_label);
        auto number_of_rnn_cell_matched = m.get_number_of_recurrent_matches();
        NGRAPH_DEBUG << " In Recurrent multi layer RNN fusion callback ";
        NGRAPH_DEBUG << "Number of RNN's Matched: " << number_of_rnn_cell_matched;
        NGRAPH_DEBUG << "matched_root: " << m.get_match_root()->get_name();
        NGRAPH_DEBUG << "src_layer_node: " << src_nodes[0]->get_name();

        //  we can fuse across different RNN layers only if SLC == DLC
        for (size_t i = 0; i < number_of_rnn_cell_matched; i++)
        {
            if (src_nodes[i]->get_shape()[1] != rnn_ht_out_nodes[i]->get_shape()[1])
            {
                NGRAPH_DEBUG << "Not fusing since the feature sizes for xt and ht_1 dont match";
                return false;
            }
        }

        // we just need to capture the input symbols {x0 | x1.....| xt} of the first lstm layer
        // the intermediate inputs for the next layer will be computed by the MKLDNN
        auto src_layer_nodes = m.get_bound_nodes_for_pattern(src_layer_label);
        auto src_layer = src_layer_nodes[src_layer_nodes.size() - 1];

        auto src_iter = compute_multi_layer_rnn_inputs(src_iter_label, m);
        auto weights_layer = compute_multi_layer_rnn_inputs(weights_layer_label, m);
        auto weights_iter = compute_multi_layer_rnn_inputs(weights_iter_label, m);
        auto bias = compute_multi_layer_rnn_inputs(bias_label, m);

        std::vector<std::shared_ptr<op::Rnn>> rnn_nodes;
        for (auto& rnn_goe_input : m.get_bound_nodes_for_pattern(rnn_ht_label))
        {
            auto rnn_op = std::dynamic_pointer_cast<op::Rnn>(rnn_goe_input->get_arguments()[0]);
            if (rnn_op)
            {
                rnn_nodes.push_back(rnn_op);
            }
            else
            {
                throw ngraph_error("Input for RNN output GetOuputElement Op should be RNN");
            }
        }

        size_t num_time_steps = rnn_nodes[0]->get_num_timesteps();
        size_t lstm_n_gates = rnn_nodes[0]->get_gates_per_cell();
        size_t batch_size = rnn_nodes[0]->get_batch_size();
        size_t sequence_len = rnn_nodes[0]->get_src_sequence_length();
        size_t src_layer_feature_size = rnn_nodes[0]->get_src_layer_feature_size();
        size_t feature_size = rnn_nodes[0]->get_src_iter_feature_size();
        size_t num_rnn_cell_states = rnn_nodes[0]->get_num_cell_states();
        size_t rnn_direction = rnn_nodes[0]->get_direction();
        size_t num_fused_rnn_layers = m.get_number_of_recurrent_matches();

        NGRAPH_DEBUG << "src_layer: " << join(src_layer->get_shape());
        NGRAPH_DEBUG << "src_iter: " << join(src_iter->get_shape());
        NGRAPH_DEBUG << "weights_layer: " << join(weights_layer->get_shape());
        NGRAPH_DEBUG << "weights_iter: " << join(weights_iter->get_shape());
        NGRAPH_DEBUG << "bias: " << join(bias->get_shape());
        NGRAPH_DEBUG << "src_seq_len: " << sequence_len;
        NGRAPH_DEBUG << "batch_size: " << batch_size;
        NGRAPH_DEBUG << "feature_size: " << feature_size;

        if ((src_layer->get_arguments().size()) != rnn_nodes[0]->get_num_timesteps() &&
            !std::dynamic_pointer_cast<op::Parameter>(src_layer))
        {
            throw ngraph_error(
                " input symbols for the layer fused RNN op, should be captured only for the first "
                "layer");
        }

        if (std::dynamic_pointer_cast<op::Parameter>(src_layer) &&
            rnn_nodes[0]->get_num_timesteps() != 1)
        {
            throw ngraph_error(
                " input symbols for the layer fused RNN op, should be captured only for the first "
                "layer");
        }

        if ((src_iter->get_arguments().size()) != num_fused_rnn_layers)
        {
            throw ngraph_error(
                "number of states(ht_1|ct_1) for RNN op in the layer fusion is not equal to num of "
                "fused_rnn_layers");
        }

        if ((weights_layer->get_arguments().size()) != num_fused_rnn_layers)
        {
            throw ngraph_error(
                "weights w.r.to input symbols of RNN op in the layer fusion is not equal to num of "
                "fused_rnn_layers");
        }

        if ((weights_iter->get_arguments().size()) != num_fused_rnn_layers)
        {
            throw ngraph_error(
                "weights w.r.to cell states of RNN op in the layer fusion is not equal to num of "
                "fused_rnn_layers");
        }

        if ((bias->get_arguments().size()) != num_fused_rnn_layers)
        {
            throw ngraph_error(
                "bias of RNN op in the layer fusion is not equal to num of fused_rnn_layers");
        }

        auto rnn = std::make_shared<op::Rnn>(src_layer,
                                             src_iter,
                                             weights_layer,
                                             weights_iter,
                                             bias,
                                             num_time_steps,
                                             lstm_n_gates,
                                             sequence_len,
                                             src_layer_feature_size,
                                             feature_size,
                                             num_rnn_cell_states,
                                             rnn_direction,
                                             num_fused_rnn_layers);

        auto layer_rnn_ht = std::make_shared<op::GetOutputElement>(rnn, 0);
        auto layer_rnn_ht_ct = std::make_shared<op::GetOutputElement>(rnn, 1);

        // multi layerd fused rnn second output {GOE1} holds the recurrent output state tensors for the last cell
        // of all the layers, we will slice the cell state output tensor {ht | ct} -> {ct} and feeds
        // {ct} consumer from the fused RNN output.
        auto ht_slice_across_layer = std::make_shared<op::Slice>(
            layer_rnn_ht_ct,
            Coordinate{0, 0},
            Coordinate{
                static_cast<unsigned long>(batch_size * rnn_direction * num_fused_rnn_layers),
                static_cast<unsigned long>(feature_size)});
        auto ct_slice_across_layer = std::make_shared<op::Slice>(
            layer_rnn_ht_ct,
            Coordinate{
                static_cast<unsigned long>(batch_size * rnn_direction * num_fused_rnn_layers), 0},
            Coordinate{
                static_cast<unsigned long>(2 * batch_size * rnn_direction * num_fused_rnn_layers),
                static_cast<unsigned long>(feature_size)});

        // Replace all the users of RNN cell state {ct} across different user.
        auto replace_rnn_output_cellstate = [&](std::shared_ptr<Node>& rnn_ct, size_t layer) {
            std::shared_ptr<Node> node_to_replace = rnn_ct;
            auto ct_slice = std::make_shared<op::Slice>(
                ct_slice_across_layer,
                Coordinate{static_cast<unsigned long>(batch_size * (layer - 1)), 0},
                Coordinate{static_cast<unsigned long>(batch_size * rnn_direction * layer),
                           static_cast<unsigned long>(feature_size)});

            if (rnn_ct->get_users().size() == 1)
            {
                if (std::dynamic_pointer_cast<op::Slice>(rnn_ct->get_users()[0]))
                {
                    node_to_replace = rnn_ct->get_users()[0];
                }
            }
            if (ngraph::is_used(node_to_replace.get()))
            {
                ngraph::replace_node(node_to_replace, ct_slice);
            }
        };

        for (size_t index = 0; index < rnn_nodes.size(); index++)
        {
            for (auto& rnn_goes : rnn_nodes[index]->get_users())
            {
                NGRAPH_DEBUG << "rnn_goes: " << rnn_goes->get_name();
                if (rnn_goes->get_users().empty())
                {
                    continue;
                }

                if (auto rnn_goe_node = std::dynamic_pointer_cast<op::GetOutputElement>(rnn_goes))
                {
                    // we need to only replace the {ht} consumers of the last RNN layer,
                    // since for other layers the intermediate outputs {ht} will be computed
                    // within MKLDNN
                    if (index == 0)
                    {
                        if (rnn_goe_node->get_n() == 0)
                        {
                            ngraph::replace_node(rnn_goes, layer_rnn_ht);
                        }
                    }
                    if (rnn_goe_node->get_n() == 1)
                    {
                        replace_rnn_output_cellstate(rnn_goes, num_fused_rnn_layers - index);
                    }
                }
            }
        }

        return true;
    };

    std::set<std::shared_ptr<pattern::op::Label>> empty_correlated_matches;
    auto m = std::make_shared<pattern::RecurrentMatcher>(
        rnn_ht_label, src_layer_label, empty_correlated_matches, callback);
    this->add_matcher(m);
}

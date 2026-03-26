#!/usr/bin/env python3
"""
Tracer Network Flow Visualizer - Smart Layout Edition
Uses topological sorting and crossing minimization for clean MSC diagrams.
"""

import re
import sys
import json
import argparse
from pathlib import Path
from typing import List, Dict, Optional, Tuple, Set
from dataclasses import dataclass, field
from collections import defaultdict


@dataclass
class Message:
    msg_id: str
    msg_type: str
    src_node: int
    dst_node: int
    content: str
    sent_step: int
    recv_step: Optional[int] = None


@dataclass
class Step:
    step_num: int
    node_id: int
    syscall: str
    syscall_args: str
    choice: Optional[int] = None
    buffer_state: Dict[int, List[Tuple[int, str, str]]] = field(default_factory=dict)
    messages_sent: List[Message] = field(default_factory=list)
    messages_recv: List[Message] = field(default_factory=list)


class TraceParser:
    def __init__(self):
        self.steps: List[Step] = []
        self.messages: Dict[str, Message] = {}
        self.msg_counter = 0
        self.switch_pattern = re.compile(r"Switched to process (\d+)")
        self.syscall_pattern = re.compile(r"^(\w+)\((.*)\)\s*=\s*(.+)")
        self.choice_pattern = re.compile(r"Using batch preset choice: (\d+)")
        self.process_pattern = re.compile(r"^\s*Process (\d+):")
        self.datagram_pattern = re.compile(
            r"\[(\d+)\]\s+from=([\d.]+):\d+,\s+len=(\d+)(?:,\s+msg=(.+))?"
        )
        self.msg_type_pattern = re.compile(r"^(\w+)\{")
        # recvfrom(3, 0xaddr"CONTENT" or recvfrom(3, 0xaddr, ... for no content
        self.recvfrom_content_pattern = re.compile(
            r'recvfrom\(\d+,\s*0x[0-9a-fA-F]+"([^"]*)"'
        )

    def parse_log(self, filepath: str):
        with open(filepath, "r") as f:
            lines = f.readlines()

        step_num = 0
        i = 0
        current_step = None

        while i < len(lines):
            line = lines[i].rstrip()

            match = self.switch_pattern.search(line)
            if match:
                if current_step:
                    self.steps.append(current_step)

                step_num += 1
                node_id = int(match.group(1))
                current_step = Step(
                    step_num=step_num, node_id=node_id, syscall="", syscall_args=""
                )
                i += 1
                continue

            if not current_step:
                i += 1
                continue

            match = self.choice_pattern.search(line)
            if match:
                current_step.choice = int(match.group(1))
                i += 1
                continue

            if not line.startswith("[") and not line.startswith("Process"):
                match = self.syscall_pattern.match(line)
                if match:
                    current_step.syscall = match.group(1)
                    current_step.syscall_args = match.group(2)

                    if current_step.syscall == "recvfrom":
                        self._detect_recvfrom(current_step, line)

                    i += 1
                    continue

            if line == "[UDP Buffers]":
                i = self._parse_udp_buffers(lines, i + 1, current_step)
                continue

            i += 1

        if current_step:
            self.steps.append(current_step)

        self._infer_message_sends()

    def _detect_recvfrom(self, step: Step, line: str):
        match = self.recvfrom_content_pattern.search(line)
        if match:
            content = match.group(1)
            msg_type = self._extract_msg_type(content)

            recv_msg = Message(
                msg_id=f"recv_{step.step_num}",
                msg_type=msg_type or "UNKNOWN",
                src_node=-1,
                dst_node=step.node_id,
                content=content,
                sent_step=-1,
                recv_step=step.step_num,
            )
            step.messages_recv.append(recv_msg)

    def _parse_udp_buffers(self, lines: List[str], start_idx: int, step: Step) -> int:
        i = start_idx
        current_node = None

        while i < len(lines):
            line = lines[i]
            stripped = line.rstrip()

            match = self.process_pattern.match(stripped)
            if match:
                current_node = int(match.group(1))
                step.buffer_state[current_node] = []
                i += 1
                continue

            match = self.datagram_pattern.search(stripped)
            if match and current_node is not None:
                from_ip = match.group(2)
                msg_content = match.group(4) if match.group(4) else ""
                from_node = self._ip_to_node(from_ip)
                msg_type = self._extract_msg_type(msg_content)

                step.buffer_state[current_node].append(
                    (from_node, msg_type or "UNKNOWN", msg_content)
                )
                i += 1
                continue

            if "(empty)" in stripped or "(empty queue)" in stripped:
                i += 1
                continue

            if (
                stripped
                and not line.startswith("  ")
                and not stripped.startswith("fd=")
            ):
                break
            if not stripped:
                i += 1
                continue

            i += 1

        return i

    def _infer_message_sends(self):
        for i in range(len(self.steps)):
            step = self.steps[i]

            if i == 0:
                continue

            prev_step = self.steps[i - 1]

            for dst_node, current_msgs in step.buffer_state.items():
                prev_msgs = prev_step.buffer_state.get(dst_node, [])
                prev_set = set((src, content) for src, _, content in prev_msgs)

                for src_node, msg_type, content in current_msgs:
                    msg_key = (src_node, content)

                    if msg_key not in prev_set:
                        msg = Message(
                            msg_id=f"msg_{self.msg_counter}",
                            msg_type=msg_type,
                            src_node=src_node,
                            dst_node=dst_node,
                            content=content,
                            sent_step=step.step_num,
                        )
                        self.msg_counter += 1
                        self.messages[msg.msg_id] = msg
                        step.messages_sent.append(msg)

            for recv_msg in step.messages_recv:
                for msg_id, msg in list(self.messages.items()):
                    if (
                        msg.dst_node == step.node_id
                        and msg.recv_step is None
                        and msg.msg_type == recv_msg.msg_type
                    ):
                        msg.recv_step = step.step_num
                        recv_msg.src_node = msg.src_node
                        recv_msg.sent_step = msg.sent_step
                        break

    def _ip_to_node(self, ip: str) -> int:
        match = re.search(r"\.(\d+)$", ip)
        if match:
            return int(match.group(1)) - 1
        return -1

    def _extract_msg_type(self, content: str) -> Optional[str]:
        match = self.msg_type_pattern.match(content)
        if match:
            return match.group(1)
        return None


class SimpleLayout:
    """
    Simple layout using global step numbers as y-axis.
    Each step gets a fixed vertical position based on its step number.
    """

    def __init__(self, messages: Dict[str, Message], max_step: int):
        self.messages = messages
        self.max_step = max_step

    def compute_layout(self) -> Tuple[Dict[int, Dict[int, float]], float]:
        """
        Returns: (node_positions, total_height)
        Uses global step number directly as y-position.
        """
        positions = {0: {}, 1: {}, 2: {}}

        # Collect all events per node
        events = {0: set(), 1: set(), 2: set()}
        for msg in self.messages.values():
            events[msg.src_node].add(msg.sent_step)
            if msg.recv_step:
                events[msg.dst_node].add(msg.recv_step)

        # Assign positions: use step number directly
        for node in range(3):
            for step_num in events[node]:
                positions[node][step_num] = float(step_num)

        # Total height based on max step
        max_y = float(self.max_step)

        return positions, max_y


class MSCVisualizer:
    """Message Sequence Chart Visualizer with Smart Layout"""

    def __init__(self, parser: TraceParser):
        self.parser = parser
        self.node_colors = ["#FF6B6B", "#4ECDC4", "#45B7D1"]

    def generate(self) -> str:
        total_msgs = len(self.parser.messages)
        delivered = sum(1 for m in self.parser.messages.values() if m.recv_step)

        # Compute simple layout using global step numbers
        max_step = (
            max(self.parser.steps, key=lambda s: s.step_num).step_num
            if self.parser.steps
            else 0
        )
        layout = SimpleLayout(self.parser.messages, max_step)
        positions, max_y_pos = layout.compute_layout()

        html = f"""<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Message Sequence Chart - Smart Layout</title>
    <style>
        body {{ 
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; 
            margin: 20px; 
            background: #0a0a14; 
            color: #e0e0e0;
        }}
        h1 {{ color: #4CAF50; text-align: center; margin-bottom: 10px; }}
        .subtitle {{ text-align: center; color: #666; margin-bottom: 20px; font-size: 14px; }}
        .container {{ 
            background: #12121e; 
            border-radius: 10px; 
            padding: 20px;
            margin: 20px 0;
            overflow-x: auto;
            box-shadow: 0 4px 20px rgba(0,0,0,0.5);
        }}
        .legend {{
            background: #1a1a2e;
            padding: 12px;
            border-radius: 8px;
            margin: 15px 0;
            display: flex;
            justify-content: center;
            gap: 25px;
            flex-wrap: wrap;
            font-size: 13px;
        }}
        .legend-item {{ display: flex; align-items: center; gap: 6px; }}
        .legend-dot {{ width: 12px; height: 12px; border-radius: 50%; }}
        .node-label {{
            font-size: 16px;
            font-weight: bold;
            fill: #fff;
            text-anchor: middle;
        }}
        .timeline {{ stroke-width: 4; opacity: 0.8; }}
        .message-line {{
            stroke-width: 2.5;
            fill: none;
            cursor: pointer;
            transition: all 0.3s ease;
        }}
        .message-line:hover {{ 
            stroke-width: 4;
            filter: drop-shadow(0 0 4px currentColor);
        }}
        .message-line.delivered {{
            stroke: #4CAF50;
            marker-end: url(#arrowhead-delivered);
        }}
        .message-line.inflight {{
            stroke: #FFB74D;
            stroke-dasharray: 4,8;
            marker-end: url(#arrowhead-inflight);
        }}
        .message-label {{
            font-size: 11px;
            fill: #ccc;
            pointer-events: none;
            font-family: monospace;
        }}
        .event-dot {{
            cursor: pointer;
            transition: r 0.2s;
        }}
        .event-dot:hover {{ r: 6; }}
        .hover-info {{
            position: fixed;
            background: rgba(15, 15, 25, 0.98);
            color: #fff;
            padding: 15px;
            border-radius: 10px;
            font-size: 13px;
            pointer-events: none;
            display: none;
            border: 1px solid #4CAF50;
            box-shadow: 0 8px 32px rgba(0,0,0,0.6);
            max-width: 350px;
            z-index: 1000;
            line-height: 1.5;
        }}
        .summary {{
            background: #1a1a2e;
            padding: 15px;
            border-radius: 8px;
            margin: 20px 0;
            text-align: center;
            display: flex;
            justify-content: center;
            gap: 40px;
        }}
        .summary-item {{ display: flex; flex-direction: column; align-items: center; }}
        .summary-value {{ font-size: 24px; font-weight: bold; color: #4CAF50; }}
        .summary-label {{ font-size: 12px; color: #888; margin-top: 4px; }}
    </style>
</head>
<body>
    <h1>📊 Message Sequence Chart</h1>
    <div class="subtitle">Smart Layout - Minimized Crossings</div>
    
    <div class="summary">
        <div class="summary-item">
            <div class="summary-value">{len(self.parser.steps)}</div>
            <div class="summary-label">Total Steps</div>
        </div>
        <div class="summary-item">
            <div class="summary-value">{total_msgs}</div>
            <div class="summary-label">Messages Sent</div>
        </div>
        <div class="summary-item">
            <div class="summary-value">{delivered}</div>
            <div class="summary-label">Delivered</div>
        </div>
        <div class="summary-item">
            <div class="summary-value">{total_msgs - delivered}</div>
            <div class="summary-label">In Transit</div>
        </div>
    </div>
    
    <div class="legend">
        <div class="legend-item">
            <div class="legend-dot" style="background: #4CAF50;"></div>
            <span>Message Delivered</span>
        </div>
        <div class="legend-item">
            <div class="legend-dot" style="background: #FFB74D;"></div>
            <span>In Flight</span>
        </div>
        <div class="legend-item">
            <div class="legend-dot" style="background: #FF6B6B;"></div>
            <span>Node 0</span>
        </div>
        <div class="legend-item">
            <div class="legend-dot" style="background: #4ECDC4;"></div>
            <span>Node 1</span>
        </div>
        <div class="legend-item">
            <div class="legend-dot" style="background: #45B7D1;"></div>
            <span>Node 2</span>
        </div>
    </div>
    
    <div class="container">
{self._generate_svg(positions, max_y_pos)}
    </div>
    
    <div class="hover-info" id="hoverInfo"></div>
    
    <script>
        const hoverInfo = document.getElementById('hoverInfo');
        
        document.querySelectorAll('.message-line').forEach(line => {{
            line.addEventListener('mouseenter', (e) => {{
                const type = e.target.getAttribute('data-type');
                const src = e.target.getAttribute('data-src');
                const dst = e.target.getAttribute('data-dst');
                const sent = e.target.getAttribute('data-sent');
                const recv = e.target.getAttribute('data-recv');
                const latency = e.target.getAttribute('data-latency');
                const content = e.target.getAttribute('data-content');
                
                let info = `<strong style="color: #4CAF50; font-size: 15px;">${{type}}</strong><br><hr style="border-color: #333; margin: 10px 0;">`;
                info += `<strong>From:</strong> Node ${{src}} → Node ${{dst}}<br>`;
                info += `<strong>Sent:</strong> Step ${{sent}}<br>`;
                if (recv) {{
                    info += `<strong>Received:</strong> Step ${{recv}}<br>`;
                    info += `<strong>Latency:</strong> ${{latency}} steps<br>`;
                    info += `<span style="color: #4CAF50;">● Delivered</span>`;
                }} else {{
                    info += `<span style="color: #FFB74D;">● In Transit</span>`;
                }}
                info += `<hr style="border-color: #333; margin: 10px 0;">`;
                info += `<div style="color: #aaa; font-size: 11px; word-break: break-all;">${{content}}</div>`;
                
                hoverInfo.innerHTML = info;
                hoverInfo.style.display = 'block';
            }});
            
            line.addEventListener('mousemove', (e) => {{
                const x = e.clientX + 15;
                const y = e.clientY + 15;
                hoverInfo.style.left = x + 'px';
                hoverInfo.style.top = y + 'px';
            }});
            
            line.addEventListener('mouseleave', () => {{
                hoverInfo.style.display = 'none';
            }});
        }});
    </script>
</body>
</html>"""
        return html

    def _generate_svg(
        self, positions: Dict[int, Dict[int, float]], max_y_pos: float
    ) -> str:
        num_nodes = 3

        margin_left = 120
        margin_top = 80
        margin_bottom = 80
        node_spacing = 350
        y_scale = 25  # Pixels per unit

        # Calculate dimensions
        svg_width = margin_left * 2 + (num_nodes - 1) * node_spacing
        svg_height = margin_top + margin_bottom + int(max_y_pos * y_scale) + 100

        svg = f'<svg width="{svg_width}" height="{svg_height}" xmlns="http://www.w3.org/2000/svg">\n'

        # Definitions for arrowheads
        svg += """    <defs>
        <marker id="arrowhead-delivered" markerWidth="12" markerHeight="8" refX="10" refY="4" orient="auto">
            <polygon points="0 0, 12 4, 0 8" fill="#4CAF50" />
        </marker>
        <marker id="arrowhead-inflight" markerWidth="12" markerHeight="8" refX="10" refY="4" orient="auto">
            <polygon points="0 0, 12 4, 0 8" fill="#FFB74D" />
        </marker>
    </defs>
"""

        node_x = [margin_left + i * node_spacing for i in range(num_nodes)]

        # Draw timelines
        for i, (x, color) in enumerate(zip(node_x, self.node_colors)):
            svg += f'    <line x1="{x}" y1="{margin_top}" x2="{x}" y2="{svg_height - margin_bottom}" class="timeline" stroke="{color}" />\n'
            svg += f'    <text x="{x}" y="{margin_top - 40}" class="node-label" fill="{color}">Node {i}</text>\n'
            svg += f'    <text x="{x}" y="{svg_height - margin_bottom + 40}" class="node-label" fill="{color}">Node {i}</text>\n'

        # Draw event markers
        for node_id in range(num_nodes):
            x = node_x[node_id]
            for step_num, y_pos in positions[node_id].items():
                y = margin_top + int(y_pos * y_scale)
                svg += f'    <circle cx="{x}" cy="{y}" r="4" fill="{self.node_colors[node_id]}" class="event-dot" />\n'

        # Sort messages by global order for cleaner layering
        sorted_messages = sorted(
            self.parser.messages.values(), key=lambda m: (m.sent_step, m.src_node)
        )

        # Draw messages
        for msg in sorted_messages:
            src_x = node_x[msg.src_node]
            dst_x = node_x[msg.dst_node]

            sent_y = margin_top + int(positions[msg.src_node][msg.sent_step] * y_scale)

            if msg.recv_step and msg.recv_step in positions[msg.dst_node]:
                recv_y = margin_top + int(
                    positions[msg.dst_node][msg.recv_step] * y_scale
                )
                latency = msg.recv_step - msg.sent_step

                # Straight line
                svg += (
                    f'    <line x1="{src_x}" y1="{sent_y}" x2="{dst_x}" y2="{recv_y}" '
                )
                content_escaped = (
                    msg.content.replace('"', "&quot;")
                    .replace("<", "&lt;")
                    .replace(">", "&gt;")
                )
                svg += f'class="message-line delivered" data-type="{msg.msg_type}" data-src="{msg.src_node}" data-dst="{msg.dst_node}" data-sent="{msg.sent_step}" data-recv="{msg.recv_step}" data-latency="{latency}" data-content="{content_escaped}" />\n'

                # Label
                mid_y = (sent_y + recv_y) / 2
                label_x = (src_x + dst_x) / 2

                # Adjust label position to avoid overlapping
                offset_y = -8 if msg.dst_node > msg.src_node else 8

                svg += f'    <text x="{label_x}" y="{mid_y + offset_y}" class="message-label" text-anchor="middle">{msg.msg_type}</text>\n'
            else:
                # In-flight message - draw shorter line to avoid crossings
                short_length = 80  # Fixed short length for inflight messages
                # Calculate endpoint at fixed distance downward
                dx = dst_x - src_x
                dy = short_length
                end_y = sent_y + dy

                svg += (
                    f'    <line x1="{src_x}" y1="{sent_y}" x2="{dst_x}" y2="{end_y}" '
                )
                content_escaped = (
                    msg.content.replace('"', "&quot;")
                    .replace("<", "&lt;")
                    .replace(">", "&gt;")
                )
                svg += f'class="message-line inflight" data-type="{msg.msg_type}" data-src="{msg.src_node}" data-dst="{msg.dst_node}" data-sent="{msg.sent_step}" data-recv="" data-latency="N/A" data-content="{content_escaped}" />\n'

                mid_y = (sent_y + end_y) / 2
                label_x = (src_x + dst_x) / 2
                svg += f'    <text x="{label_x}" y="{mid_y - 5}" class="message-label" text-anchor="middle">{msg.msg_type}...</text>\n'

        svg += "</svg>"
        return svg


def generate_linear_trace(parser: TraceParser) -> str:
    """Generate a linear text trace of all send/recv events in global order."""
    events = []

    for msg in parser.messages.values():
        events.append(
            (
                msg.sent_step,
                f"SEND  Node {msg.src_node} -> Node {msg.dst_node}: [{msg.msg_type}] {msg.content}",
            )
        )
        if msg.recv_step is not None:
            events.append(
                (
                    msg.recv_step,
                    f"RECV  Node {msg.dst_node} <- Node {msg.src_node}: [{msg.msg_type}] {msg.content}",
                )
            )

    events.sort(key=lambda x: (x[0], 0 if x[1].startswith("SEND") else 1))

    lines = [f"Linear Trace - {len(parser.messages)} messages", "=" * 80]
    for step, event in events:
        lines.append(f"Step {step:4d}: {event}")

    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="Visualize network message flow from tracer logs - Smart Layout Edition"
    )
    parser.add_argument("logfile", help="Tracer log file to analyze")
    parser.add_argument("-o", "--output", help="Output file (default: stdout)")
    parser.add_argument("--json", action="store_true", help="Export as JSON")
    parser.add_argument(
        "--text",
        action="store_true",
        help="Output linear text trace of send/recv events",
    )

    args = parser.parse_args()

    log_path = Path(args.logfile)
    if not log_path.exists():
        print(f"Error: Log file not found: {args.logfile}", file=sys.stderr)
        sys.exit(1)

    print(f"Parsing {args.logfile}...", file=sys.stderr)
    trace_parser = TraceParser()
    trace_parser.parse_log(str(log_path))
    print(
        f"Parsed {len(trace_parser.steps)} steps, {len(trace_parser.messages)} messages",
        file=sys.stderr,
    )

    if args.json:
        data = {
            "messages": [
                {
                    "msg_id": m.msg_id,
                    "type": m.msg_type,
                    "src": m.src_node,
                    "dst": m.dst_node,
                    "sent_step": m.sent_step,
                    "recv_step": m.recv_step,
                }
                for m in trace_parser.messages.values()
            ]
        }
        output = json.dumps(data, indent=2)
    elif args.text:
        output = generate_linear_trace(trace_parser)
    else:
        viz = MSCVisualizer(trace_parser)
        output = viz.generate()

    if args.output:
        with open(args.output, "w") as f:
            f.write(output)
        print(f"Visualization written to: {args.output}", file=sys.stderr)
    else:
        print(output)


if __name__ == "__main__":
    main()

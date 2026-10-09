import os
import json
import logging
from typing import Dict, Any, Optional, List
import httpx

logger = logging.getLogger("gemma_engine")

class Gemma4AgentEngine:
    """
    Open-Source / Open-Weight Gemma 4 Agent Engine for Real-World Store Operations.
    Built for Track 2 (Best Open-Source AI Project - PS 03).
    
    Powers:
    1. Semantic Intent Classification
    2. Tool Selection & Workflow Orchestration
    3. Failure Handling & Self-Correction
    4. Grounded Conversational Voice & Text Synthesis
    """

    def __init__(self, base_url: Optional[str] = None, model_name: Optional[str] = None):
        # Default to local Ollama or friend's Ollama host
        self.base_url = (
            base_url
            or os.environ.get("OLLAMA_BASE_URL")
            or os.environ.get("GEMMA_BASE_URL")
            or "http://localhost:11434/v1"
        ).rstrip("/")
        
        self.model_name = (
            model_name
            or os.environ.get("GEMMA_MODEL")
            or "gemma4:e4b"
        )
        self.timeout = float(os.environ.get("GEMMA_TIMEOUT", "15.0"))

    async def is_available(self) -> bool:
        """Checks if the Ollama / Gemma 4 endpoint is reachable."""
        try:
            async with httpx.AsyncClient(timeout=2.0) as client:
                resp = await client.get(f"{self.base_url}/models")
                return resp.status_code == 200
        except Exception:
            return False

    async def _chat_completion(
        self,
        messages: List[Dict[str, str]],
        temperature: float = 0.2,
        response_format: Optional[Dict[str, str]] = None
    ) -> Optional[str]:
        """Sends an OpenAI-compatible chat completion request to the Gemma 4 server."""
        payload: Dict[str, Any] = {
            "model": self.model_name,
            "messages": messages,
            "temperature": temperature,
            "stream": False
        }
        if response_format:
            payload["response_format"] = response_format

        try:
            async with httpx.AsyncClient(timeout=self.timeout) as client:
                resp = await client.post(
                    f"{self.base_url}/chat/completions",
                    json=payload,
                    headers={"Content-Type": "application/json"}
                )
                if resp.status_code == 200:
                    data = resp.json()
                    choices = data.get("choices", [])
                    if choices and "message" in choices[0]:
                        return choices[0]["message"].get("content", "").strip()
                else:
                    logger.warning(f"[Gemma4] HTTP {resp.status_code}: {resp.text[:200]}")
        except Exception as e:
            logger.warning(f"[Gemma4] Connection notice to {self.base_url}: {e}")
        return None

    def _extract_json(self, raw_text: str) -> Optional[Dict[str, Any]]:
        """Resilient JSON extractor handling markdown fences and surrounding commentary."""
        if not raw_text:
            return None
        text = raw_text.strip()
        # Strip ```json ... ```
        if "```" in text:
            parts = text.split("```")
            for p in parts:
                p_clean = p.strip()
                if p_clean.startswith("json"):
                    p_clean = p_clean[4:].strip()
                if p_clean.startswith("{") and p_clean.endswith("}"):
                    try:
                        return json.loads(p_clean)
                    except Exception:
                        pass
        # Find first { and last }
        start = text.find("{")
        end = text.rfind("}")
        if start != -1 and end != -1 and end > start:
            try:
                return json.loads(text[start:end + 1])
            except Exception:
                pass
        return None

    async def classify_intent(self, prompt: str) -> Optional[str]:
        """
        Classifies the retail merchant's spoken request into one of the 4 operational workflows
        using Gemma 4 open-weight model.
        """
        system_prompt = (
            "You are the Open-Source OperatorOS AI Dispatcher powered by Google Gemma 4.\n"
            "Classify the retail merchant's request into exactly one intent category:\n"
            "- CLOSE_SHOP: store closing, cash drawer reconciliation, safe drop, supplier reorders (CALL-E), placing dairy/bakery orders.\n"
            "- CUSTOMER_DUES: checking overdue customer credit/khata/udhaar, sending Telegram/WhatsApp payment reminders, recovering bills.\n"
            "- WEEKEND_SALES: launching weekend promotions, campaigns, discounts, WhatsApp broadcasts, organic bundles, flash sales.\n"
            "- ASK_SALES: asking about daily sales numbers, transactions, cash status, inventory inquiries, or general store queries.\n\n"
            "Return valid JSON only: {\"intent\": \"CLOSE_SHOP\" | \"CUSTOMER_DUES\" | \"WEEKEND_SALES\" | \"ASK_SALES\"}"
        )
        user_msg = f"Merchant says: \"{prompt}\""

        raw = await self._chat_completion(
            messages=[
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_msg}
            ],
            temperature=0.0,
            response_format={"type": "json_object"}
        )

        if raw:
            parsed = self._extract_json(raw)
            if parsed and parsed.get("intent") in ("CLOSE_SHOP", "CUSTOMER_DUES", "WEEKEND_SALES", "ASK_SALES"):
                return parsed["intent"]
        return None

    async def generate_conversational_reply(
        self,
        user_prompt: str,
        sales_data: Dict[str, Any],
        profile_data: Dict[str, Any],
        inv_data: Dict[str, Any],
        dues_data: Dict[str, Any]
    ) -> Optional[Dict[str, str]]:
        """
        Generates a human-like, warm, and grounded voice response using Gemma 4,
        rooted entirely in live database facts.
        """
        store_name = profile_data.get("name", "Green Valley Organic Grocers")
        gross_sales = int(sales_data.get("total_sales_amount", 0))
        tx_count = sales_data.get("total_transactions", 0)
        cash_drawer = int(profile_data.get("current_cash_in_drawer", 0))
        float_target = int(profile_data.get("opening_cash", 5000))
        low_stock_items = [it.get("name", "") for it in inv_data.get("low_stock_items", [])]
        overdue_amt = int(dues_data.get("total_overdue_amount", 0))
        overdue_cnt = dues_data.get("overdue_customers_count", 0)

        system_prompt = (
            f"You are OperatorOS, the open-source voice-first AI store manager powered by Google Gemma 4.\n"
            f"You are speaking directly to Rajesh-ji, owner of {store_name} in Indiranagar, Bengaluru.\n\n"
            "Live Store Database Telemetry:\n"
            f"- Gross Sales Today: Rs {gross_sales} across {tx_count} transactions\n"
            f"- Cash in Drawer: Rs {cash_drawer} (with Rs {float_target} opening float target)\n"
            f"- Low Stock Alerts: {len(low_stock_items)} items ({', '.join(low_stock_items[:3])}) below reorder cutoff\n"
            f"- Overdue Khata Dues: Rs {overdue_amt} across {overdue_cnt} customer accounts\n\n"
            "Instructions:\n"
            "1. voice_text: 2 concise spoken sentences answering Rajesh-ji directly. DO NOT use markdown symbols, emojis, or rupee symbols—say 'rupees'. Do NOT start with 'Namaste' or 'Hello'.\n"
            "2. markdown: Formatted markdown card matching your spoken answer.\n\n"
            "Respond in JSON format: {\"voice_text\": \"...\", \"markdown\": \"...\"}"
        )

        raw = await self._chat_completion(
            messages=[
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_prompt}
            ],
            temperature=0.3,
            response_format={"type": "json_object"}
        )

        if raw:
            parsed = self._extract_json(raw)
            if parsed and parsed.get("voice_text") and parsed.get("markdown"):
                return parsed
        return None

    async def handle_tool_failure(
        self,
        tool_name: str,
        error_message: str,
        context: Dict[str, Any]
    ) -> Dict[str, Any]:
        """
        Hackathon PS 03 Core Requirement: Appropriate Tool Use & Failure Handling.
        When a business MCP tool encounters a network issue, constraint violation, or API rate limit,
        Gemma 4 analyzes the root cause and provides a graceful fallback plan.
        """
        system_prompt = (
            "You are the Open-Source Gemma 4 Resilience & Failure Recovery Handler for retail operations.\n"
            "A business tool execution encountered an error. Analyze the error and determine:\n"
            "1. recovery_action: 'RETRY' | 'USE_LOCAL_FALLBACK' | 'NOTIFY_MERCHANT'\n"
            "2. merchant_voice_explanation: 1 clear spoken sentence reassuring the merchant what fallback was applied.\n"
            "3. technical_remediation: Steps taken to maintain store operations.\n\n"
            "Respond in JSON format only."
        )
        user_msg = (
            f"Failed Tool: {tool_name}\n"
            f"Error Details: {error_message}\n"
            f"Store Context: {json.dumps(context)}"
        )

        raw = await self._chat_completion(
            messages=[
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_msg}
            ],
            temperature=0.1,
            response_format={"type": "json_object"}
        )

        if raw:
            parsed = self._extract_json(raw)
            if parsed:
                return parsed

        # Deterministic fallback if model offline
        return {
            "recovery_action": "USE_LOCAL_FALLBACK",
            "merchant_voice_explanation": f"I encountered an issue reaching {tool_name}, so I activated our local offline database ledger to keep your store running.",
            "technical_remediation": f"Local SQLite/file-based fallback engaged for {tool_name}."
        }

# Global singleton instance
gemma_engine = Gemma4AgentEngine()

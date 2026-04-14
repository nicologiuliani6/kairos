class KairosCompileError(Exception):
    def __init__(self, stage, message):
        self.stage = stage
        self.message = message
        super().__init__(f"[{stage}] {message}")

    def __str__(self):
        return f"[{self.stage}] {self.message}"

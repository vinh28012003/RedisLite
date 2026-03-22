variable "cluster_name" {
  description = "Name of the kind cluster"
  type        = string
  default     = "redislite"
}

variable "wait_for_ready" {
  description = "Block until the cluster API server is responsive"
  type        = bool
  default     = true
}

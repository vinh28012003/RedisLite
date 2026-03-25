variable "region" {
  description = "AWS region for all resources"
  type        = string
  default     = "us-east-1"
}

variable "cluster_name" {
  description = "Name prefix for EKS cluster, VPC, and related resources"
  type        = string
  default     = "redislite"
}

variable "node_instance_type" {
  description = "EC2 instance type for EKS worker nodes"
  type        = string
  default     = "t3.small"
}

variable "node_desired_count" {
  description = "Number of worker nodes in the managed node group"
  type        = number
  default     = 2
}

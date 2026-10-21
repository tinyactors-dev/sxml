<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="text"/>

  <xsl:template match="violations[violation]">
    <xsl:text>Your change breaks rules in AGENTS.md.</xsl:text>
    <xsl:text> Fix these:&#10;</xsl:text>
    <xsl:apply-templates select="violation"/>
  </xsl:template>

  <xsl:template match="violation">
    <xsl:text>{@file}:{@line}: </xsl:text>
    <xsl:apply-templates select="*"/>
    <xsl:text>&#10;</xsl:text>
  </xsl:template>

  <xsl:template match="max-parameters">
    <xsl:text>{../@name} takes {../@count} parameters;</xsl:text>
    <xsl:text> the limit is {@limit}.</xsl:text>
  </xsl:template>

  <xsl:template match="only-calls">
    <xsl:text>calls {@function}, which only code</xsl:text>
    <xsl:text> in {@directory} may call.</xsl:text>
  </xsl:template>

  <xsl:template match="no-calls">
    <xsl:text>calls {@function}, which code</xsl:text>
    <xsl:text> in {@directory} must not call.</xsl:text>
  </xsl:template>
</xsl:stylesheet>

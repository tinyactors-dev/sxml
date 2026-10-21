<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns="http://www.w3.org/2000/svg">
  <xsl:output method="xml" indent="yes" omit-xml-declaration="yes"/>
  <xsl:param name="unit" select="40"/>
  <xsl:template match="paths">
    <svg viewBox="0 0 520 {count(path) * 32}">
      <xsl:apply-templates select="path"/>
    </svg>
  </xsl:template>
  <xsl:template match="path">
    <g transform="translate(0 {(position() - 1) * 32})">
      <text y="20">{@name}</text>
      <rect class="requests" x="130" y="6" height="18"
            width="{@requests * $unit}"/>
      <rect class="errors" x="130" y="6" height="18"
            width="{@errors * $unit}"/>
      <text x="{138 + @requests * $unit}" y="20">{@requests} requests, {@slowest} ms max</text>
    </g>
  </xsl:template>
</xsl:stylesheet>
